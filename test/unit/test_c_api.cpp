//
// The C embedding surface.
//
// The important test here is not that the calls work -- it is that going
// through C produces THE SAME BYTES as going through C++. A host embeds this
// library through the C header, the goldens are rendered through C++, and if
// those two ever diverge the gate stops describing what ships.
//
#include <cstring>
#include <string>
#include <vector>

#include "bindings/c_api.h"
#include "sfx/bank.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_engine.h"
#include "sfx/sfx_json.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

const char* kProj =
    "{\"version\":1,\"name\":\"capi\","
    "\"buses\":[{\"name\":\"world\",\"maxInstances\":8},"
    "{\"name\":\"ui\",\"spatial\":false,\"maxInstances\":2}],"
    "\"sounds\":[{\"name\":\"hit\",\"bus\":\"world\",\"priority\":200,"
    "\"maxInstances\":4,\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":[{"
    "\"osc\":\"noise\",\"noiseColor\":\"pink\","
    "\"ampEnv\":{\"attack\":0,\"decay\":90,\"sustain\":0,\"release\":5},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":3000,\"resonance\":0.4}}]}}]},"
    "{\"name\":\"click\",\"bus\":\"ui\",\"priority\":250,"
    "\"variants\":[{\"patch\":{\"oneShot\":true,\"layers\":[{"
    "\"osc\":\"wavetable\","
    "\"wavetable\":{\"generator\":\"additiveHarmonicMorph\",\"frameCount\":1,"
    "\"params\":{\"startHarmonics\":4,\"falloff\":\"square\"}},"
    "\"pitchNote\":84,"
    "\"ampEnv\":{\"attack\":0,\"decay\":40,\"sustain\":0,\"release\":3}}]}}]}]}";

constexpr i32 U(int n) { return n * GAIN_ONE; }

// The same script, played twice: once through the C header, once through the
// C++ classes the goldens use.
struct Trigger {
    u64 at_block;
    const char* sound;
    u32 seed;
    i32 x, y, z;
};

const Trigger kScript[] = {
    {0, "hit", 111, U(2), U(1), 0},
    {12, "click", 222, 0, 0, 0},
    {30, "hit", 333, -U(6), U(4), 0},
    {60, "hit", 444, U(14), -U(3), 0},
};

std::vector<i16> render_through_c(int blocks) {
    dsnd_ctx* ctx = dsnd_create();
    char report[2048] = {};
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
    CHECK(dsnd_build_cache(ctx, 0, report, sizeof(report)) == 1);
    dsnd_set_listener(ctx, 0, 0, U(1), 0);

    std::vector<i16> out(static_cast<size_t>(blocks) * CONTROL_BLOCK * 2, 0);
    size_t next = 0;
    for (int b = 0; b < blocks; ++b) {
        while (next < sizeof(kScript) / sizeof(kScript[0]) &&
               kScript[next].at_block == static_cast<u64>(b)) {
            const Trigger& t = kScript[next];
            dsnd_play(ctx, dsnd_sound_id(ctx, t.sound), -1, t.seed, t.x, t.y, t.z);
            ++next;
        }
        dsnd_render(ctx, out.data() + static_cast<size_t>(b) * CONTROL_BLOCK * 2, CONTROL_BLOCK);
    }
    dsnd_destroy(ctx);
    return out;
}

std::vector<i16> render_through_cpp(int blocks) {
    WaveTableCache wtc;
    SoundBank bank;
    CHECK(load_bank_json(kProj, std::strlen(kProj), wtc, bank).ok);
    PcmCache cache;
    CHECK(cache.build(bank, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
    SfxEngine engine;
    engine.set_content(&bank, &cache);

    {
        SfxCommand c;
        c.type = SfxCommand::Type::SetListener;
        c.a = 0;
        c.b = 0;
        c.c = U(1);
        c.e = 0;
        engine.queue().push(c);
    }

    std::vector<i16> out(static_cast<size_t>(blocks) * CONTROL_BLOCK * 2, 0);
    size_t next = 0;
    for (int b = 0; b < blocks; ++b) {
        while (next < sizeof(kScript) / sizeof(kScript[0]) &&
               kScript[next].at_block == static_cast<u64>(b)) {
            const Trigger& t = kScript[next];
            SfxCommand c;
            c.type = SfxCommand::Type::Play;
            c.handle = engine.next_handle();
            c.a = bank.sound_index(t.sound);
            c.b = t.x;
            c.c = t.y;
            c.d = t.z;
            c.e = t.seed;
            c.variant = -1;
            engine.queue().push(c);
            ++next;
        }
        engine.render(out.data() + static_cast<size_t>(b) * CONTROL_BLOCK * 2, CONTROL_BLOCK);
    }
    return out;
}

}  // namespace

TEST(c_api_renders_bit_for_bit_like_the_cpp_path) {
    const auto a = render_through_c(160);
    const auto b = render_through_cpp(160);
    CHECK_EQ(a.size(), b.size());
    int mismatches = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) ++mismatches;
    CHECK_EQ(mismatches, 0);

    // Control: it is real audio being compared, not two silences.
    i32 peak = 0;
    for (i16 s : a) {
        const i32 v = s < 0 ? -static_cast<i32>(s) : s;
        if (v > peak) peak = v;
    }
    CHECK(peak > 200);
}

TEST(c_api_lifecycle_is_safe_in_any_order) {
    // Nothing here should crash, and nothing should be audible.
    dsnd_ctx* ctx = dsnd_create();
    std::vector<i16> out(CONTROL_BLOCK * 2, 0);

    // Render before a bank: silence, and commands are drained rather than
    // queued up to fire all at once when one arrives.
    dsnd_play(ctx, 0, -1, 1, 0, 0, 0);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    for (i16 s : out) CHECK_EQ(s, 0);

    char report[1024] = {};
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
    CHECK_EQ(dsnd_sound_count(ctx), 2);
    CHECK_EQ(dsnd_bus_count(ctx), 2);
    CHECK(dsnd_sound_id(ctx, "hit") >= 0);
    CHECK_EQ(dsnd_sound_id(ctx, "nope"), -1);
    CHECK(std::string(dsnd_sound_name(ctx, dsnd_sound_id(ctx, "hit"))) == "hit");
    CHECK(dsnd_sound_name(ctx, 999) == nullptr);

    // Unload with something playing, then render: the engine must have let go
    // of the bank's patches rather than reading freed memory.
    const u32 h = dsnd_play(ctx, dsnd_sound_id(ctx, "hit"), -1, 7, 0, 0, 0);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    CHECK(dsnd_alive(ctx, h));
    dsnd_unload_bank(ctx);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    CHECK(!dsnd_alive(ctx, h));
    CHECK_EQ(dsnd_sound_count(ctx), 0);

    // Reload and go again.
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
    CHECK(dsnd_play(ctx, dsnd_sound_id(ctx, "hit"), -1, 9, 0, 0, 0) != 0);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);

    dsnd_destroy(ctx);
    dsnd_destroy(nullptr);  // must be a no-op, not a crash
}

TEST(c_api_null_context_is_a_no_op_everywhere) {
    // A host that failed to create a context should get silence, not a fault.
    CHECK_EQ(dsnd_sound_id(nullptr, "x"), -1);
    CHECK_EQ(dsnd_sound_count(nullptr), 0);
    CHECK_EQ(dsnd_alive(nullptr, 1), 0);
    CHECK_EQ(dsnd_music_duck_milli(nullptr), 1000);
    CHECK_EQ(dsnd_now(nullptr), 0);
    dsnd_set_listener(nullptr, 0, 0, 0, 0);
    dsnd_stop(nullptr, 1, 0);
    dsnd_stop_all(nullptr, 0);
    dsnd_render(nullptr, nullptr, 64);
    CHECK_EQ(dsnd_play(nullptr, 0, -1, 1, 0, 0, 0), 0u);
    dsnd_diag diag;
    dsnd_diag_get(nullptr, &diag);
    CHECK_EQ(diag.played, 0);
}

TEST(c_api_reports_why_a_play_was_refused) {
    dsnd_ctx* ctx = dsnd_create();
    char report[2048] = {};
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
    CHECK(dsnd_build_cache(ctx, 0, report, sizeof(report)) == 1);

    std::vector<i16> out(CONTROL_BLOCK * 2, 0);
    const int hit = dsnd_sound_id(ctx, "hit");
    for (int i = 0; i < 12; ++i) {
        dsnd_play(ctx, hit, -1, static_cast<u32>(i + 1), 0, 0, 0);
        dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    }
    dsnd_diag d;
    dsnd_diag_get(ctx, &d);
    CHECK_EQ(d.played, 4);                  // maxInstances is 4
    CHECK_EQ(d.rejected_max_instances, 8);  // and the rest said so
    CHECK(d.cached_variants > 0);
    CHECK(d.cache_bytes > 0);

    // An unknown id is counted, not crashed on.
    dsnd_play(ctx, 4242, -1, 1, 0, 0, 0);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    dsnd_diag_get(ctx, &d);
    CHECK_EQ(d.rejected_unknown, 1);
    dsnd_destroy(ctx);
}

TEST(c_api_reports_a_report_on_a_bad_bank) {
    dsnd_ctx* ctx = dsnd_create();
    char report[1024] = {};
    const char* bad = "{\"version\":1,\"sounds\":[]}";
    CHECK_EQ(dsnd_load_bank_json(ctx, bad, std::strlen(bad), report, sizeof(report)), 0);
    CHECK(std::string(report).find("error") != std::string::npos);
    dsnd_destroy(ctx);
}

TEST(c_api_ducking_crosses_to_the_host) {
    const char* ducker =
        "{\"version\":1,\"buses\":[{\"name\":\"w\"}],\"sounds\":[{\"name\":\"boom\","
        "\"duckMusic\":{\"db\":-12,\"ms\":300},\"variants\":[{\"patch\":{\"oneShot\":true,"
        "\"layers\":[{\"osc\":\"noise\",\"ampEnv\":{\"attack\":0,\"decay\":400,"
        "\"sustain\":0,\"release\":5}}]}}]}]}";
    dsnd_ctx* ctx = dsnd_create();
    char report[1024] = {};
    CHECK(dsnd_load_bank_json(ctx, ducker, std::strlen(ducker), report, sizeof(report)) == 1);
    CHECK_EQ(dsnd_music_duck_milli(ctx), 1000);

    std::vector<i16> out(CONTROL_BLOCK * 2, 0);
    dsnd_play(ctx, dsnd_sound_id(ctx, "boom"), -1, 1, 0, 0, 0);
    dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    const int ducked = dsnd_music_duck_milli(ctx);
    CHECK(ducked < 1000);
    CHECK(ducked > 0);

    for (int i = 0; i < 300; ++i) dsnd_render(ctx, out.data(), CONTROL_BLOCK);
    CHECK_EQ(dsnd_music_duck_milli(ctx), 1000);
    dsnd_destroy(ctx);
}

// The contract says frames must be a multiple of 64. Passing something else
// must not desynchronise the engine from its own clock -- the remainder is
// filled with silence and the clock advances by whole blocks only.
TEST(c_api_a_partial_block_does_not_move_the_clock) {
    dsnd_ctx* ctx = dsnd_create();
    char report[1024] = {};
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);

    std::vector<i16> out(200 * 2, 0);
    dsnd_render(ctx, out.data(), 100);  // one whole block, then 36 frames over
    CHECK_EQ(dsnd_now(ctx), static_cast<u64>(CONTROL_BLOCK));
    for (int i = CONTROL_BLOCK; i < 100; ++i) {
        CHECK_EQ(out[static_cast<size_t>(i) * 2], 0);
        CHECK_EQ(out[static_cast<size_t>(i) * 2 + 1], 0);
    }
    dsnd_destroy(ctx);
}

// The path a game actually takes: a baked .sfxb, not JSON. It must resolve
// the same ids and render the same bytes -- otherwise the gate (which renders
// from .sfxproj) is testing something other than what ships.
TEST(c_api_loads_a_baked_bank_identically) {
    std::vector<u8> baked;
    CHECK(bake_bank(kProj, std::strlen(kProj), baked).ok);
    CHECK(baked.size() > 100);

    auto render = [&](bool from_binary) {
        dsnd_ctx* ctx = dsnd_create();
        char report[2048] = {};
        if (from_binary)
            CHECK(dsnd_load_bank(ctx, baked.data(), baked.size(), report, sizeof(report)) == 1);
        else
            CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
        CHECK(dsnd_build_cache(ctx, 0, report, sizeof(report)) == 1);
        dsnd_set_listener(ctx, 0, 0, U(1), 0);

        std::vector<i16> out(120 * CONTROL_BLOCK * 2, 0);
        size_t next = 0;
        for (int b = 0; b < 120; ++b) {
            while (next < sizeof(kScript) / sizeof(kScript[0]) &&
                   kScript[next].at_block == static_cast<u64>(b)) {
                const Trigger& t = kScript[next];
                dsnd_play(ctx, dsnd_sound_id(ctx, t.sound), -1, t.seed, t.x, t.y, t.z);
                ++next;
            }
            dsnd_render(ctx, out.data() + static_cast<size_t>(b) * CONTROL_BLOCK * 2,
                        CONTROL_BLOCK);
        }
        dsnd_destroy(ctx);
        return out;
    };

    const auto from_json = render(false);
    const auto from_sfxb = render(true);
    CHECK_EQ(from_json.size(), from_sfxb.size());
    int mismatches = 0;
    for (size_t i = 0; i < from_json.size(); ++i)
        if (from_json[i] != from_sfxb[i]) ++mismatches;
    CHECK_EQ(mismatches, 0);

    i32 peak = 0;
    for (i16 s : from_sfxb) {
        const i32 v = s < 0 ? -static_cast<i32>(s) : s;
        if (v > peak) peak = v;
    }
    CHECK(peak > 200);
}

// A corrupt bank must be refused with a report, not loaded half-way.
TEST(c_api_refuses_a_corrupt_baked_bank) {
    std::vector<u8> baked;
    CHECK(bake_bank(kProj, std::strlen(kProj), baked).ok);
    baked[baked.size() - 5] ^= 0x02;

    dsnd_ctx* ctx = dsnd_create();
    char report[1024] = {};
    CHECK_EQ(dsnd_load_bank(ctx, baked.data(), baked.size(), report, sizeof(report)), 0);
    CHECK(std::string(report).find("error") != std::string::npos);
    CHECK_EQ(dsnd_sound_count(ctx), 0);

    // And it is still usable afterwards: a failed load must leave a clean
    // context, not a half-populated one.
    CHECK(dsnd_load_bank_json(ctx, kProj, std::strlen(kProj), report, sizeof(report)) == 1);
    CHECK_EQ(dsnd_sound_count(ctx), 2);
    dsnd_destroy(ctx);
}
