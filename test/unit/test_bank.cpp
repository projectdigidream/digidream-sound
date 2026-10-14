//
// The bank format: .sfxproj parses, .sfxb round-trips, and a damaged bank is
// refused rather than half-read.
//
#include <cmath>
#include <algorithm>
#include <cstring>
#include <string>

#include "output/sha256.h"
#include "sfx/bank.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_engine.h"
#include "sfx/sfx_json.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

// One layer, a plain sine with a short percussive envelope. Deliberately the
// smallest patch the loader accepts, so these tests fail for format reasons
// rather than synthesis ones.
const char* kPatch =
    "{\"layers\":[{\"osc\":\"wavetable\",\"wave\":{\"gen\":\"additiveHarmonicMorph\","
    "\"frames\":[{\"harmonics\":[1]}]},"
    "\"ampEnv\":{\"attackMs\":1,\"decayMs\":80,\"sustain\":0,\"releaseMs\":10}}]}";

std::string proj_with(const std::string& sound_body) {
    return std::string(
               "{\"version\":1,\"name\":\"test\","
               "\"buses\":[{\"name\":\"sfx\",\"maxInstances\":8},"
               "{\"name\":\"ui\",\"spatial\":false,\"gainDb\":-3}],"
               "\"sounds\":[") +
           sound_body + "]}";
}

std::string simple_proj() {
    return proj_with(std::string("{\"name\":\"door_open\",\"bus\":\"sfx\",\"priority\":200,"
                                 "\"maxInstances\":2,\"cooldownMs\":40,\"gainDb\":-2,"
                                 "\"variants\":[{\"patch\":") +
                     kPatch + "}]}");
}

}  // namespace

TEST(bank_json_loads_a_minimal_project) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = simple_proj();
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK_EQ(b.buses.size(), 2u);
    CHECK_EQ(b.sounds.size(), 1u);
    CHECK_EQ(b.variants.size(), 1u);
    CHECK_EQ(b.patches.size(), 1u);
    CHECK(b.sounds[0].name == "door_open");
    CHECK_EQ(b.sounds[0].priority, 200);
    CHECK_EQ(b.sounds[0].max_instances, 2);
    // 40 ms at 750 Hz control rate is 30 blocks.
    CHECK_EQ(b.sounds[0].cooldown_blocks, 30u);
    // A gesture defaults to exactly one note at the start.
    CHECK_EQ(b.variants[0].gesture.size(), 1u);
    CHECK_EQ(b.variants[0].gesture[0].at_blocks, 0u);
}

TEST(bank_json_resolves_bus_names_to_indices) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = proj_with(std::string("{\"name\":\"click\",\"bus\":\"ui\",\"variants\":"
                                                "[{\"patch\":") +
                                    kPatch + "}]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(r.ok);
    CHECK_EQ(b.sounds[0].bus, 1);
    CHECK(!b.buses[1].spatial);
}

TEST(bank_json_unknown_bus_warns_and_falls_back) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = proj_with(std::string("{\"name\":\"x\",\"bus\":\"nope\",\"variants\":"
                                                "[{\"patch\":") +
                                    kPatch + "}]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(r.ok);  // a mistyped bus is not worth refusing a bank over
    CHECK(!r.warnings.empty());
    CHECK_EQ(b.sounds[0].bus, 0);
}

TEST(bank_json_loop_implies_live) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = proj_with(std::string("{\"name\":\"wind\",\"loop\":true,\"variants\":"
                                                "[{\"patch\":") +
                                    kPatch + "}]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(r.ok);
    CHECK(b.sounds[0].live);
    CHECK(!r.warnings.empty());
}

TEST(bank_json_wide_pitch_jitter_implies_live) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j =
        proj_with(std::string("{\"name\":\"swoop\",\"pitchJitterCents\":900,\"variants\":"
                              "[{\"patch\":") +
                  kPatch + "}]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(r.ok);
    CHECK(b.sounds[0].live);
}

TEST(bank_json_rejects_a_sound_with_no_variants) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = proj_with("{\"name\":\"empty\",\"variants\":[]}");
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(!r.ok);
}

TEST(bank_json_rejects_duplicate_names) {
    WaveTableCache cache;
    SoundBank b;
    const std::string one = std::string("{\"name\":\"dup\",\"variants\":[{\"patch\":") + kPatch +
                            "}]}";
    const std::string j = proj_with(one + "," + one);
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(!r.ok);
    CHECK_EQ(b.sounds.size(), 1u);
}

TEST(bank_json_rejects_a_newer_major_version) {
    WaveTableCache cache;
    SoundBank b;
    const std::string j = "{\"version\":2,\"sounds\":[]}";
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(!r.ok);
}

TEST(bank_json_dedupes_identical_patches) {
    WaveTableCache cache;
    SoundBank b;
    const std::string a = std::string("{\"name\":\"a\",\"variants\":[{\"patch\":") + kPatch + "}]}";
    const std::string c = std::string("{\"name\":\"b\",\"variants\":[{\"patch\":") + kPatch + "}]}";
    const std::string j = proj_with(a + "," + c);
    LoadResult r = load_bank_json(j.c_str(), j.size(), cache, b);
    CHECK(r.ok);
    CHECK_EQ(b.sounds.size(), 2u);
    CHECK_EQ(b.variants.size(), 2u);
    // Two sounds, one stored patch: the second variant points at the first.
    CHECK_EQ(b.patches.size(), 1u);
    CHECK_EQ(b.variants[0].patch_index, b.variants[1].patch_index);
}

// ---------------------------------------------------------------- .sfxb

TEST(sfxb_round_trips) {
    const std::string j = simple_proj();
    std::vector<u8> bytes;
    LoadResult br = bake_bank(j.c_str(), j.size(), bytes);
    for (const auto& e : br.errors) std::printf("  bake error: %s\n", e.c_str());
    CHECK(br.ok);
    CHECK(bytes.size() > 100);

    WaveTableCache cache;
    SoundBank b;
    BankMeta m;
    LoadResult lr = load_bank(bytes.data(), bytes.size(), cache, b, &m);
    for (const auto& e : lr.errors) std::printf("  load error: %s\n", e.c_str());
    CHECK(lr.ok);

    CHECK_EQ(b.buses.size(), 2u);
    CHECK_EQ(b.sounds.size(), 1u);
    CHECK(b.sounds[0].name == "door_open");
    CHECK_EQ(b.sounds[0].priority, 200);
    CHECK_EQ(b.sounds[0].max_instances, 2);
    CHECK_EQ(b.sounds[0].cooldown_blocks, 30u);
    CHECK_EQ(b.sounds[0].variant_count, 1);
    CHECK_EQ(b.variants[0].gesture.size(), 1u);
    CHECK_EQ(m.sound_count, 1u);
    CHECK(m.name == "test");
    CHECK(!m.has_render_hash);
}

// The resolved policy must be the same whichever door it came through --
// otherwise the tool and the game disagree about what a bank means.
TEST(sfxb_matches_the_json_it_was_baked_from) {
    const std::string j = simple_proj();
    WaveTableCache c1;
    SoundBank from_json;
    CHECK(load_bank_json(j.c_str(), j.size(), c1, from_json).ok);

    std::vector<u8> bytes;
    CHECK(bake_bank(j.c_str(), j.size(), bytes).ok);
    WaveTableCache c2;
    SoundBank from_bin;
    CHECK(load_bank(bytes.data(), bytes.size(), c2, from_bin).ok);

    CHECK_EQ(from_json.sounds.size(), from_bin.sounds.size());
    for (size_t i = 0; i < from_json.sounds.size(); ++i) {
        const SoundDef& a = from_json.sounds[i];
        const SoundDef& b = from_bin.sounds[i];
        CHECK(a.name == b.name);
        CHECK_EQ(a.bus, b.bus);
        CHECK_EQ(a.gain_q16, b.gain_q16);
        CHECK_EQ(a.priority, b.priority);
        CHECK_EQ(a.max_instances, b.max_instances);
        CHECK_EQ(a.cooldown_blocks, b.cooldown_blocks);
        CHECK_EQ(a.pitch_jitter_cents_q16, b.pitch_jitter_cents_q16);
        CHECK_EQ(a.atten.min_dist_q16, b.atten.min_dist_q16);
        CHECK_EQ(a.atten.max_dist_q16, b.atten.max_dist_q16);
        CHECK_EQ(static_cast<int>(a.atten.curve), static_cast<int>(b.atten.curve));
        CHECK_EQ(a.variant_first, b.variant_first);
        CHECK_EQ(a.variant_count, b.variant_count);
    }
    CHECK_EQ(from_json.patch_json.size(), from_bin.patch_json.size());
    for (size_t i = 0; i < from_json.patch_json.size(); ++i)
        CHECK(from_json.patch_json[i] == from_bin.patch_json[i]);
}

TEST(sfxb_rejects_a_corrupted_payload) {
    const std::string j = simple_proj();
    std::vector<u8> bytes;
    CHECK(bake_bank(j.c_str(), j.size(), bytes).ok);

    // Flip one bit deep in the payload. The seal must catch it.
    bytes[bytes.size() - 8] ^= 0x01;
    WaveTableCache cache;
    SoundBank b;
    LoadResult r = load_bank(bytes.data(), bytes.size(), cache, b);
    CHECK(!r.ok);
    CHECK_EQ(b.sounds.size(), 0u);
}

TEST(sfxb_rejects_bad_magic_and_truncation) {
    const std::string j = simple_proj();
    std::vector<u8> bytes;
    CHECK(bake_bank(j.c_str(), j.size(), bytes).ok);

    {
        std::vector<u8> bad = bytes;
        bad[0] ^= 0xFF;
        WaveTableCache cache;
        SoundBank b;
        CHECK(!load_bank(bad.data(), bad.size(), cache, b).ok);
    }
    {
        std::vector<u8> bad(bytes.begin(), bytes.begin() + 40);
        WaveTableCache cache;
        SoundBank b;
        CHECK(!load_bank(bad.data(), bad.size(), cache, b).ok);
    }
    {
        // A newer major version is refused outright, rather than read
        // optimistically and misinterpreted.
        std::vector<u8> bad = bytes;
        bad[4] = 99;
        WaveTableCache cache;
        SoundBank b;
        CHECK(!load_bank(bad.data(), bad.size(), cache, b).ok);
    }
}

TEST(sfxb_carries_a_render_seal_once_stamped) {
    const std::string j = simple_proj();
    std::vector<u8> bytes;
    CHECK(bake_bank(j.c_str(), j.size(), bytes).ok);

    BankMeta before;
    CHECK(read_bank_meta(bytes.data(), bytes.size(), before).ok);
    CHECK(!before.has_render_hash);

    u8 hash[32];
    for (int i = 0; i < 32; ++i) hash[i] = static_cast<u8>(i * 7 + 1);
    CHECK(seal_bank(bytes, hash, "oneshot.sfxscript"));

    BankMeta after;
    LoadResult r = read_bank_meta(bytes.data(), bytes.size(), after);
    CHECK(r.ok);  // resealed, so the payload hash must still verify
    CHECK(after.has_render_hash);
    CHECK(after.render_script == "oneshot.sfxscript");
    for (int i = 0; i < 32; ++i) CHECK_EQ(after.render_hash[i], hash[i]);
    // Sealing must not disturb what the bank says about itself.
    CHECK(after.name == before.name);
    CHECK_EQ(after.sound_count, before.sound_count);
    for (int i = 0; i < 32; ++i) CHECK_EQ(after.content_hash[i], before.content_hash[i]);

    // And the bank still loads.
    WaveTableCache cache;
    SoundBank b;
    CHECK(load_bank(bytes.data(), bytes.size(), cache, b).ok);
}

// A section this build does not know about must be ignored, not fatal --
// that is what lets a newer baker add one without a flag day.
TEST(sfxb_ignores_an_unknown_section) {
    const std::string j = simple_proj();
    std::vector<u8> bytes;
    CHECK(bake_bank(j.c_str(), j.size(), bytes).ok);

    WaveTableCache c0;
    SoundBank ref;
    CHECK(load_bank(bytes.data(), bytes.size(), c0, ref).ok);

    // Rebuild the frame with one extra section appended to the table and its
    // body appended to the payload, then re-seal by hand.
    const size_t table_at = 4 + 2 + 2 + 64 + 4;
    u32 nsecs = 0;
    for (int i = 0; i < 4; ++i) nsecs |= static_cast<u32>(bytes[4 + 2 + 2 + 64 + i]) << (8 * i);
    const size_t payload_at = table_at + 12u * nsecs;
    std::vector<u8> payload(bytes.begin() + payload_at, bytes.end());
    const u32 extra_off = static_cast<u32>(payload.size());
    const u8 extra[4] = {'h', 'i', '!', 0};
    payload.insert(payload.end(), extra, extra + 4);

    std::vector<u8> out(bytes.begin(), bytes.begin() + payload_at);
    // bump the section count
    for (int i = 0; i < 4; ++i)
        out[4 + 2 + 2 + 64 + i] = static_cast<u8>((nsecs + 1) >> (8 * i));
    // append the new table entry (tag "ZZZZ")
    const u32 tag = 'Z' | ('Z' << 8) | ('Z' << 16) | (static_cast<u32>('Z') << 24);
    for (u32 v : {tag, extra_off, 4u})
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
    // re-seal
    const std::string hex = msynth::sha256_hex(payload.data(), payload.size());
    for (int i = 0; i < 64; ++i) out[8 + i] = static_cast<u8>(hex[i]);
    out.insert(out.end(), payload.begin(), payload.end());

    WaveTableCache cache;
    SoundBank b;
    LoadResult r = load_bank(out.data(), out.size(), cache, b);
    for (const auto& e : r.errors) std::printf("  error: %s\n", e.c_str());
    CHECK(r.ok);
    CHECK_EQ(b.sounds.size(), ref.sounds.size());
    CHECK(b.sounds[0].name == "door_open");
}

// ---------------------------------------------------------------------------
// Chorus and delay sends.
//
// These were parsed and then thrown away with a warning for the whole life of
// the format so far, so the interesting properties are (a) that they now reach
// the engine, and (b) that adding them did not change what every bank written
// before today means.
// ---------------------------------------------------------------------------

TEST(bank_json_reads_chorus_and_delay_sends) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",")
        + "\"buses\":[{\"name\":\"a\",\"sendChorus\":0.4,\"sendDelay\":0.25}],"
        + "\"fx\":{\"chorus\":{\"rateHz\":1.2,\"depthMs\":4,\"wetGain\":0.3}},"
        + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":" + kPatch + "}]}]}";

    WaveTableCache c;
    SoundBank b;
    LoadResult r = load_bank_json(doc.c_str(), doc.size(), c, b);
    CHECK(r.ok);
    // No warning now that they are wired -- the old one said they were ignored.
    for (const auto& w : r.warnings) CHECK(w.find("not wired") == std::string::npos);
    CHECK(b.buses[0].send_chorus_q16 > 0);
    CHECK(b.buses[0].send_delay_q16 > 0);
    CHECK(b.chorus.wet_gain > 0);
}

TEST(chorus_send_changes_the_render) {
    // The point of the effect: the same sound through a bus with a chorus send
    // must not render identically to one without. A send that reached nothing
    // would still load, still warn about nothing, and still be silent about it.
    // NOT kPatch: that one is deliberately minimal for FORMAT tests and uses
    // spellings the loader accepts and ignores, so it renders silence -- and
    // two silences are equal however much chorus you send them through.
    const char* audible =
        "{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\",\"noiseColor\":\"white\","
        "\"mixGain\":0.9,"
        "\"ampEnv\":{\"attack\":0,\"decay\":200,\"sustain\":0,\"release\":20}}]}";

    auto render_hash = [&](const std::string& bus_extra, i64* energy) {
        const std::string doc =
            std::string("{\"version\":1,\"name\":\"t\",")
            + "\"buses\":[{\"name\":\"a\",\"spatial\":false" + bus_extra + "}],"
            + "\"fx\":{\"chorus\":{\"rateHz\":1.1,\"depthMs\":5,\"wetGain\":0.5}},"
            + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":" + audible + "}]}]}";
        WaveTableCache c;
        SoundBank b;
        LoadResult r = load_bank_json(doc.c_str(), doc.size(), c, b);
        CHECK(r.ok);
        PcmCache cache;
        CacheReport crep;
        cache.build(b, 4u << 20, &crep);
        SfxEngine e;
        e.set_content(&b, &cache);
        SfxCommand cmd;
        cmd.type = SfxCommand::Type::Play;
        cmd.handle = e.next_handle();
        cmd.a = 0;
        e.queue().push(cmd);
        std::vector<i16> pcm(48000 * 2, 0);
        for (int at = 0; at < 48000; at += 512) e.render(pcm.data() + at * 2, 512);
        *energy = 0;
        for (i16 v : pcm) *energy += v < 0 ? -v : v;
        return msynth::sha256_hex(pcm.data(), pcm.size() * sizeof(i16));
    };

    i64 e_dry = 0, e_wet = 0;
    const std::string dry = render_hash("", &e_dry);
    const std::string wet = render_hash(",\"sendChorus\":0.6", &e_wet);
    // Guard the guard: if neither render made a sound, "they differ" would be
    // a test of nothing.
    CHECK(e_dry > 0);
    CHECK(dry != wet);
    CHECK(e_wet > e_dry);   // a wet send adds energy, it does not replace it
}

TEST(bank_bake_round_trips_chorus_and_delay) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",")
        + "\"buses\":[{\"name\":\"a\",\"sendChorus\":0.4,\"sendDelay\":0.25}],"
        + "\"fx\":{\"chorus\":{\"rateHz\":1.2,\"depthMs\":4,\"wetGain\":0.3},"
        + "\"delay\":{\"timeMs\":220,\"feedback\":0.4,\"wetGain\":0.2}},"
        + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":" + kPatch + "}]}]}";

    std::vector<u8> baked;
    LoadResult br = bake_bank(doc.c_str(), doc.size(), baked);
    CHECK(br.ok);

    WaveTableCache c;
    SoundBank b;
    LoadResult lr = load_bank(baked.data(), baked.size(), c, b);
    CHECK(lr.ok);
    CHECK(b.buses[0].send_chorus_q16 > 0);
    CHECK(b.buses[0].send_delay_q16 > 0);
    CHECK(b.chorus.wet_gain > 0);
    CHECK(b.delay.wet_gain > 0);
    CHECK(b.delay.feedback_q16 > 0);
}

TEST(a_bank_with_no_sends_bakes_without_the_new_section) {
    // BSND is written only when something uses it, so a bank that predates the
    // feature bakes to the bytes it always did. That is what makes the section
    // safe to add without a version bump: nothing that did not ask for it sees
    // any difference at all.
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"a\"}],")
        + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":" + kPatch + "}]}]}";
    std::vector<u8> baked;
    CHECK(bake_bank(doc.c_str(), doc.size(), baked).ok);

    bool found = false;
    for (size_t i = 0; i + 4 <= baked.size(); ++i)
        if (std::memcmp(baked.data() + i, "BSND", 4) == 0) found = true;
    CHECK(!found);

    WaveTableCache c;
    SoundBank b;
    CHECK(load_bank(baked.data(), baked.size(), c, b).ok);
    CHECK(b.buses[0].send_chorus_q16 == 0);
    CHECK(b.buses[0].send_delay_q16 == 0);
}

// ---------------------------------------------------------------------------
// Modal resonators.
//
// An exciter feeding a bank of damped resonators -- the one standard technique
// in the procedural-audio literature this engine had no form of. The patch is
// the exciter, and this is the body it rings.
// ---------------------------------------------------------------------------

TEST(resonators_ring_long_after_the_exciter_stops) {
    // A 4 ms click into a 700 ms mode must produce a 700 ms sound. This is the
    // whole point: the tail comes from the body, not from the exciter, so the
    // instance must stay alive while a mode still has energy in it.
    const char* click =
        "{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\",\"noiseColor\":\"white\","
        "\"mixGain\":1,\"ampEnv\":{\"attack\":0,\"decay\":4,\"sustain\":0,\"release\":0}}]}";

    auto frames_of = [&](const std::string& reson) {
        const std::string doc =
            std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\",\"spatial\":false}],")
            + "\"sounds\":[{\"name\":\"s\"," + reson
            + "\"variants\":[{\"patch\":" + click + "}]}]}";
        WaveTableCache c;
        SoundBank b;
        LoadResult r = load_bank_json(doc.c_str(), doc.size(), c, b);
        CHECK(r.ok);
        std::vector<i32> pcm;
        u32 got = 0;
        render_variant(b, 0, 0, false, 4 * 750, pcm, &got, nullptr, nullptr);
        return got;
    };

    const u32 dry = frames_of("");
    const u32 rung = frames_of(
        "\"resonators\":[{\"hz\":220,\"decayMs\":700,\"gain\":1.2}],");
    std::printf("  dry %u frames, rung %u frames\n", dry, rung);
    CHECK(dry < 2000);          // the click alone is milliseconds
    CHECK(rung > 20000);        // the body is hundreds of milliseconds
}

TEST(resonators_are_baked_and_reloaded) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\"}],")
        + "\"sounds\":[{\"name\":\"s\",\"resonatorDry\":0.25,"
        + "\"resonators\":[{\"hz\":180,\"decayMs\":600,\"gain\":1.5},"
        + "{\"hz\":740,\"decayMs\":200,\"gain\":0.4}],"
        + "\"variants\":[{\"patch\":" + kPatch + "}]}]}";

    std::vector<u8> baked;
    CHECK(bake_bank(doc.c_str(), doc.size(), baked).ok);

    WaveTableCache c;
    SoundBank b;
    CHECK(load_bank(baked.data(), baked.size(), c, b).ok);
    CHECK(b.sounds[0].resonators.count == 2);
    CHECK(b.sounds[0].resonators.modes[0].hz_q16 == 180 * GAIN_ONE);
    CHECK(b.sounds[0].resonators.modes[1].decay_ms_q16 == 200 * GAIN_ONE);
    CHECK(b.sounds[0].resonators.dry_q16 == GAIN_ONE / 4);
}

TEST(a_bank_with_no_resonators_bakes_without_the_new_section) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"a\"}],")
        + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":" + kPatch + "}]}]}";
    std::vector<u8> baked;
    CHECK(bake_bank(doc.c_str(), doc.size(), baked).ok);
    bool found = false;
    for (size_t i = 0; i + 4 <= baked.size(); ++i)
        if (std::memcmp(baked.data() + i, "RESO", 4) == 0) found = true;
    CHECK(!found);
}

// THE property, with a resonator in the path.
//
// The body is rung in render_sources' LIVE branch only, so the modes are baked
// into the cached samples and cached playback does not ring them a second
// time. If that were ever wrong, a cached sound would have its body applied
// twice and a live one once -- and the goldens would not notice, because none
// of them uses a resonator.
TEST(a_rung_sound_is_the_same_cached_or_live) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\",\"spatial\":false}],")
        + "\"sounds\":[{\"name\":\"s\",\"resonatorDry\":0.4,"
        + "\"resonators\":[{\"hz\":300,\"decayMs\":250,\"gain\":1.0},"
        + "{\"hz\":905,\"decayMs\":120,\"gain\":0.6}],"
        + "\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\","
        + "\"noiseColor\":\"white\",\"mixGain\":1,"
        + "\"ampEnv\":{\"attack\":0,\"decay\":6,\"sustain\":0,\"release\":0}}]}}]}]}";

    auto render = [&](bool live) {
        WaveTableCache c;
        SoundBank b;
        CHECK(load_bank_json(doc.c_str(), doc.size(), c, b).ok);
        if (live) for (auto& s : b.sounds) s.live = true;
        PcmCache cache;
        CacheReport crep;
        cache.build(b, 8u << 20, &crep);
        SfxEngine e;
        e.set_content(&b, &cache);
        SfxCommand cmd;
        cmd.type = SfxCommand::Type::Play;
        cmd.handle = e.next_handle();
        cmd.a = 0;
        e.queue().push(cmd);
        std::vector<i16> pcm(48000, 0);
        for (int at = 0; at < 24000; at += 512) e.render(pcm.data() + at * 2, 512);
        return msynth::sha256_hex(pcm.data(), pcm.size() * sizeof(i16));
    };

    const std::string cached = render(false);
    const std::string live = render(true);
    CHECK(cached == live);
}

// A rung sound must END.
//
// A fixed-point two-pole does not decay to exact zero on its own: asr floors,
// so a small state settles at z1 = z2 = 1 and stays there -- a limit cycle.
// Because an instance stays alive while a mode is ringing, that was not a
// quiet tail but a live slot that was never reclaimed, and the cache refusing
// the sound outright after 4000 ms. It shipped in DOOM's pinky melee, whose
// longest mode is 202 ms, and none of the tests above noticed because their
// modes happened to land on exact zero.
//
// This uses that sound's actual fitted body.
TEST(a_rung_sound_ends_and_its_instance_is_reclaimed) {
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\",\"spatial\":false}],")
        + "\"sounds\":[{\"name\":\"s\",\"resonatorDry\":0.6,\"resonators\":["
        + "{\"hz\":94.8441,\"decayMs\":201.577,\"gain\":0.995395},"
        + "{\"hz\":308.641,\"decayMs\":134.026,\"gain\":0.799864},"
        + "{\"hz\":328.215,\"decayMs\":83.6492,\"gain\":0.599987},"
        + "{\"hz\":400.008,\"decayMs\":52.1716,\"gain\":0.550918}],"
        + "\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\","
        + "\"noiseColor\":\"pink\",\"mixGain\":1,"
        + "\"ampEnv\":{\"attack\":0,\"decay\":8,\"sustain\":0,\"release\":0}}]}}]}]}";

    WaveTableCache c;
    SoundBank b;
    CHECK(load_bank_json(doc.c_str(), doc.size(), c, b).ok);

    // Offline: the render must finish, and well inside the cache's 4 s cap.
    std::vector<i32> pcm;
    u32 frames = 0;
    const bool ended = render_variant(b, 0, 0, false, 4 * 750, pcm, &frames, nullptr, nullptr);
    std::printf("  pinky body ends after %u frames (%.0f ms)\n", frames,
                static_cast<double>(frames) / 48.0);
    CHECK(ended);
    CHECK(frames < 48000);   // a 202 ms body is over in well under a second

    // Live: the instance must be released, not held open by a mode that never
    // reaches zero. This is the leak.
    for (auto& s : b.sounds) s.live = true;
    PcmCache cache;
    CacheReport crep;
    cache.build(b, 8u << 20, &crep);
    SfxEngine e;
    e.set_content(&b, &cache);
    SfxCommand cmd;
    cmd.type = SfxCommand::Type::Play;
    cmd.handle = e.next_handle();
    cmd.a = 0;
    e.queue().push(cmd);
    std::vector<i16> out(512 * 2);
    for (int blk = 0; blk < 2 * 750 / 8; ++blk) e.render(out.data(), 512);   // 2 s
    CHECK(e.diag().instances == 0);
    CHECK(e.diag().retiring == 0);
}

// A mode's `gain` is the amplitude it rings at when struck, and its `decayMs`
// is how long that ring takes to fall 60 dB -- for every mode, however long or
// low.
//
// The first resonator did not normalise: `gain: 1` on a long low mode meant
// roughly +80 dB, and every DOOM role the fitter won with `modal` came out
// saturated at exactly +42.1 dBFS. The second normalised to the steady-state
// sine response, which bounded that but left long modes nearly silent when
// struck. This pins both numbers to what the format says they mean, across the
// corners where each attempt was worst.
TEST(a_struck_mode_rings_at_its_stated_gain_and_decay) {
    msynth::tables_init();
    struct Case { int hz; int decay_ms; float gain; };
    const Case cases[] = {{95, 200, 1.0f}, {40, 2000, 1.0f}, {440, 300, 0.5f},
                          {3000, 80, 2.0f}, {200, 4000, 1.0f}};
    for (const Case& k : cases) {
        ResonatorBank b;
        b.count = 1;
        b.modes[0] = {k.hz << GAIN_SHIFT, k.decay_ms << GAIN_SHIFT,
                      static_cast<i32>(k.gain * GAIN_ONE)};
        b.dry_q16 = 0;
        ResonatorState st;
        st.prepare(b);

        // One impulse at half full scale, then silence.
        const double amp = static_cast<double>(1 << 23);
        const size_t n = static_cast<size_t>(k.decay_ms) * 48 + 48000;
        std::vector<double> out;
        out.reserve(n);
        for (size_t at = 0; at < n; at += CONTROL_BLOCK) {
            i64 l[CONTROL_BLOCK] = {}, r[CONTROL_BLOCK] = {};
            if (at == 0) l[0] = r[0] = static_cast<i64>(amp);
            st.process(l, r, CONTROL_BLOCK);
            for (int i = 0; i < CONTROL_BLOCK; ++i) out.push_back(static_cast<double>(l[i]));
        }
        // The ring's first-cycle peak is the struck amplitude.
        const size_t period = static_cast<size_t>(48000 / k.hz) + 2;
        double first = 0;
        for (size_t i = 0; i < period && i < out.size(); ++i) first = std::max(first, std::fabs(out[i]));
        // And one period's peak AT decayMs must be 60 dB below it.
        const size_t at = static_cast<size_t>(k.decay_ms) * 48;
        double later = 0;
        for (size_t i = at; i < at + period && i < out.size(); ++i)
            later = std::max(later, std::fabs(out[i]));
        const double g = first / amp;
        const double drop = 20.0 * std::log10(std::max(later, 1.0) / first);
        std::printf("  %5d Hz %5d ms gain %.2f -> struck %.3f, %.1f dB at decayMs\n",
                    k.hz, k.decay_ms, static_cast<double>(k.gain), g, drop);
        CHECK(g > k.gain * 0.9 && g < k.gain * 1.1);
        CHECK(drop < -55.0 && drop > -65.0);
    }
}

TEST(a_layer_delayed_past_the_others_still_speaks) {
    // A 20 ms click, and a second layer that starts 70 ms later. The render
    // used to end on the silence after the click -- the delayed layer was
    // still counting down, so it had produced nothing yet -- and because the
    // cache is built by this render, the game never played it either.
    auto frames_of = [&](const char* layers) {
        const std::string doc =
            std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\",\"spatial\":false}],")
            + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":["
            + layers + "]}}]}]}";
        WaveTableCache c;
        SoundBank b;
        CHECK(load_bank_json(doc.c_str(), doc.size(), c, b).ok);
        std::vector<i32> pcm;
        u32 got = 0;
        render_variant(b, 0, 0, false, 4 * 750, pcm, &got, nullptr, nullptr);
        return got;
    };
    const char* click =
        "{\"osc\":\"noise\",\"mixGain\":0.8,\"ampEnv\":{\"attack\":0,\"decay\":20,\"sustain\":0,\"release\":0}}";
    const char* late =
        "{\"osc\":\"noise\",\"mixGain\":0.8,\"startDelayMs\":70,"
        "\"ampEnv\":{\"attack\":0,\"decay\":80,\"sustain\":0,\"release\":5}}";
    const u32 both = frames_of((std::string(click) + "," + late).c_str());
    const u32 alone = frames_of(late);
    std::printf("  click + late %u frames, late alone %u frames\n", both, alone);
    CHECK(both > 48 * 120);    // past 70 ms + most of its 80 ms decay
    CHECK(alone > 48 * 120);   // a sound that is ONLY a delayed layer speaks too
}

TEST(a_reused_pool_renders_what_a_fresh_one_does) {
    // The editor and the fitter render through ONE long-lived pool. A render
    // ends on silence, which can leave a voice inaudible but mid-envelope; if
    // reset() kept that envelope level, the next sound in that slot started
    // its attack part-way up and the same patch rendered differently the
    // second time. What `roar` does is enough: a slow attack, and an LFO on
    // the amplitude deep enough to reach zero -- the first trough is silence,
    // the render ends there, and the envelope is left half-way through.
    const std::string doc =
        std::string("{\"version\":1,\"name\":\"t\",\"buses\":[{\"name\":\"b\",\"spatial\":false}],")
        + "\"sounds\":[{\"name\":\"s\",\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":["
        + "{\"osc\":\"noise\",\"mixGain\":0.8,\"ampEnv\":{\"attack\":120,\"decay\":900,\"sustain\":0,\"release\":0},"
        + "\"lfos\":[{\"shape\":\"sine\",\"rateHz\":6}],"
        + "\"modMatrix\":[{\"src\":\"LFO1\",\"dst\":\"Amplitude\",\"amount\":1.5}]}"
        + "]}}]}]}";
    WaveTableCache c;
    SoundBank b;
    CHECK(load_bank_json(doc.c_str(), doc.size(), c, b).ok);
    std::vector<i32> fresh, again;
    u32 n1 = 0, n2 = 0;
    render_variant(b, 0, 0, false, 4 * 750, fresh, &n1, nullptr, nullptr);
    SfxVoicePool pool;
    render_variant(b, 0, 0, false, 4 * 750, again, &n2, nullptr, &pool);
    render_variant(b, 0, 0, false, 4 * 750, again, &n2, nullptr, &pool);
    CHECK(n1 == n2);
    CHECK(fresh == again);
}
