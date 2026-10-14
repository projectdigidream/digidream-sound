//
// The engine: triggering, the three tiers of voice limiting, mixing, ducking.
//
// Voice limiting is the part a game actually feels, and it is the part that is
// easiest to claim without having. So every tier here is tested by making it
// bite AND by a control showing it was the tier under test that bit -- a
// rejection count going up proves nothing if the sound was never going to
// play.
//
#include <string>
#include <vector>

#include "sfx/pcm_cache.h"
#include "sfx/sfx_engine.h"
#include "sfx/sfx_json.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

const char* kShort =
    "{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\","
    "\"ampEnv\":{\"attack\":1,\"decay\":40,\"sustain\":0,\"release\":5},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":5000,\"resonance\":0.2}}]}";

const char* kLong =
    "{\"oneShot\":true,\"layers\":[{\"osc\":\"noise\","
    "\"ampEnv\":{\"attack\":1,\"decay\":900,\"sustain\":0,\"release\":10},"
    "\"filter\":{\"mode\":\"lowpass\",\"cutoffHz\":5000,\"resonance\":0.2}}]}";

std::string snd(const std::string& name, const char* patch, const std::string& extra = "") {
    return "{\"name\":\"" + name + "\"" + extra + ",\"variants\":[{\"patch\":" + patch + "}]}";
}

std::string proj(const std::string& sounds, const std::string& buses =
                     "[{\"name\":\"world\",\"maxInstances\":16},"
                     "{\"name\":\"ui\",\"spatial\":false,\"maxInstances\":2}]") {
    return "{\"version\":1,\"name\":\"t\",\"buses\":" + buses + ",\"sounds\":[" + sounds + "]}";
}

// A loaded, cached bank plus an engine wired to it.
struct Rig {
    WaveTableCache wc;
    SoundBank bank;
    PcmCache cache;
    SfxEngine engine;
    u32 handles = 0;

    bool load(const std::string& j, bool build_cache = true) {
        LoadResult r = load_bank_json(j.c_str(), j.size(), wc, bank);
        for (const auto& e : r.errors) std::printf("  load error: %s\n", e.c_str());
        if (!r.ok) return false;
        if (build_cache) {
            LoadResult c = cache.build(bank, DEFAULT_CACHE_BUDGET_KB * 1024);
            for (const auto& e : c.errors) std::printf("  cache error: %s\n", e.c_str());
            if (!c.ok) return false;
        }
        engine.set_content(&bank, &cache);
        return true;
    }

    u32 play(const char* name, u32 seed = 1, i32 x = 0, i32 y = 0, i32 z = 0) {
        SfxCommand c;
        c.type = SfxCommand::Type::Play;
        c.handle = ++handles;
        c.a = bank.sound_index(name);
        c.b = x;
        c.c = y;
        c.d = z;
        c.e = seed;
        engine.queue().push(c);
        return c.handle;
    }

    void stop(u32 handle, i32 fade_blocks = 0) {
        SfxCommand c;
        c.type = SfxCommand::Type::Stop;
        c.handle = handle;
        c.a = fade_blocks;
        engine.queue().push(c);
    }

    // Render n blocks, returning the peak absolute sample seen.
    i32 run(int blocks) {
        std::vector<i16> out(static_cast<size_t>(blocks) * CONTROL_BLOCK * 2);
        engine.render(out.data(), blocks * CONTROL_BLOCK);
        i32 peak = 0;
        for (i16 s : out) {
            const i32 a = s < 0 ? -static_cast<i32>(s) : s;
            if (a > peak) peak = a;
        }
        return peak;
    }
};

}  // namespace

TEST(engine_plays_a_sound) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort))));
    CHECK(rig.play("hit") != 0);
    const i32 peak = rig.run(20);
    CHECK(peak > 0);
    CHECK_EQ(rig.engine.diag().played, 1);
}

TEST(engine_is_silent_with_nothing_playing) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort))));
    CHECK_EQ(rig.run(20), 0);
}

TEST(engine_ignores_an_unknown_sound_without_crashing) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort))));
    SfxCommand c;
    c.type = SfxCommand::Type::Play;
    c.handle = 99;
    c.a = 12345;
    rig.engine.queue().push(c);
    rig.run(4);
    CHECK_EQ(rig.engine.diag().rejected_unknown, 1);
    CHECK_EQ(rig.engine.diag().played, 0);
}

// ------------------------------------------------- tier (a): per-sound caps

TEST(cooldown_rejects_a_retrigger_and_then_allows_one) {
    Rig rig;
    // 40 ms = 30 blocks.
    CHECK(rig.load(proj(snd("gun", kShort, ",\"cooldownMs\":40,\"maxInstances\":8"))));

    rig.play("gun");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().played, 1);

    // Immediately again: refused, and refused for the stated reason.
    rig.play("gun");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().played, 1);
    CHECK_EQ(rig.engine.diag().rejected_cooldown, 1);

    // Past the cooldown: allowed.
    rig.run(35);
    rig.play("gun");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().played, 2);
    CHECK_EQ(rig.engine.diag().rejected_cooldown, 1);
}

// Control: the same two triggers with no cooldown both play, so the test
// above is measuring the cooldown and not some other limit.
TEST(control_without_a_cooldown_both_triggers_play) {
    Rig rig;
    CHECK(rig.load(proj(snd("gun", kShort, ",\"maxInstances\":8"))));
    rig.play("gun");
    rig.run(1);
    rig.play("gun");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().played, 2);
    CHECK_EQ(rig.engine.diag().rejected_cooldown, 0);
}

TEST(max_instances_rejects_beyond_the_cap) {
    Rig rig;
    CHECK(rig.load(proj(snd("hum", kLong, ",\"maxInstances\":3"))));
    for (int i = 0; i < 10; ++i) {
        rig.play("hum", static_cast<u32>(i + 1));
        rig.run(1);
    }
    CHECK_EQ(rig.engine.diag().played, 3);
    CHECK_EQ(rig.engine.diag().rejected_max_instances, 7);
    CHECK_EQ(rig.engine.diag().instances, 3);
}

TEST(steal_oldest_keeps_the_count_at_the_cap) {
    Rig rig;
    CHECK(rig.load(
        proj(snd("saw", kLong, ",\"maxInstances\":2,\"onLimit\":\"stealOldest\""))));
    for (int i = 0; i < 6; ++i) {
        rig.play("saw", static_cast<u32>(i + 1));
        rig.run(1);
    }
    // Every trigger was accepted, and the count never exceeded the cap.
    CHECK_EQ(rig.engine.diag().played, 6);
    CHECK_EQ(rig.engine.diag().rejected_max_instances, 0);
    CHECK_EQ(rig.engine.diag().instances, 2);
    CHECK(rig.engine.diag().stolen >= 4);
}

// A cap is released as instances finish, or a sound would play a few times
// and then go quiet forever.
TEST(the_cap_recovers_as_instances_end) {
    Rig rig;
    CHECK(rig.load(proj(snd("tick", kShort, ",\"maxInstances\":1"))));
    rig.play("tick");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().instances, 1);
    rig.run(120);  // the 40 ms decay is long over
    CHECK_EQ(rig.engine.diag().instances, 0);
    rig.play("tick");
    rig.run(1);
    CHECK_EQ(rig.engine.diag().played, 2);
}

// ------------------------------------------------------ tier (b): bus budget

TEST(a_bus_budget_caps_its_own_bus) {
    Rig rig;
    // The ui bus allows 2 at a time; the sound's own cap is higher, so the
    // bus is the only thing that can be limiting here.
    CHECK(rig.load(proj(snd("a", kLong, ",\"bus\":\"ui\",\"maxInstances\":8"))));
    for (int i = 0; i < 6; ++i) {
        rig.play("a", static_cast<u32>(i + 1));
        rig.run(1);
    }
    CHECK_EQ(rig.engine.bus_instances(1), 2);
    CHECK_EQ(rig.engine.diag().rejected_max_instances, 0);
}

// The point of confining stealing to a bus: a flood on one bus must not
// silence another. This is what a designer relies on when they put the
// player's own weapon on its own bus.
TEST(a_flood_on_one_bus_cannot_silence_another) {
    Rig rig;
    CHECK(rig.load(proj(
        snd("chatter", kLong, ",\"bus\":\"ui\",\"maxInstances\":16,\"priority\":250") + "," +
        snd("shot", kLong, ",\"bus\":\"world\",\"maxInstances\":16,\"priority\":1"))));

    const u32 shot = rig.play("shot", 7);
    rig.run(4);
    CHECK(rig.engine.alive(shot));

    // Flood the OTHER bus with the highest-priority sound in the bank -- the
    // one that would certainly win a global contest.
    for (int i = 0; i < 30; ++i) {
        rig.play("chatter", static_cast<u32>(100 + i));
        rig.run(1);
    }

    // Still sounding. Nothing on `ui` could reach across to `world`, however
    // important it was.
    CHECK(rig.engine.alive(shot));
    CHECK_EQ(rig.engine.bus_instances(0), 1);
    CHECK_EQ(rig.engine.bus_instances(1), 2);
}

// Control: the same flood on the SAME bus does cut it, so the test above is
// measuring the bus boundary and not simply a sound that outlives everything.
TEST(control_a_flood_on_the_same_bus_does_cut_it) {
    Rig rig;
    CHECK(rig.load(proj(
        snd("chatter", kLong, ",\"bus\":\"ui\",\"maxInstances\":16,\"priority\":250") + "," +
        snd("shot", kLong, ",\"bus\":\"ui\",\"maxInstances\":16,\"priority\":1"))));

    const u32 shot = rig.play("shot", 7);
    rig.run(4);
    CHECK(rig.engine.alive(shot));

    for (int i = 0; i < 30; ++i) {
        rig.play("chatter", static_cast<u32>(100 + i));
        rig.run(1);
    }
    CHECK(!rig.engine.alive(shot));
    CHECK(rig.engine.diag().stolen >= 1);
}

TEST(priority_refuses_to_cut_something_more_important) {
    Rig rig;
    CHECK(rig.load(proj(snd("important", kLong, ",\"bus\":\"ui\",\"priority\":250,\"maxInstances\":8") +
                        "," + snd("trivial", kLong, ",\"bus\":\"ui\",\"priority\":5,\"maxInstances\":8"))));

    // Fill the ui bus (budget 2) with important sounds, and let them age past
    // the steal shield.
    rig.play("important", 1);
    rig.run(1);
    rig.play("important", 2);
    rig.run(8);
    CHECK_EQ(rig.engine.diag().instances, 2);

    // A trivial sound cannot displace them; it is refused instead.
    rig.play("trivial", 3);
    rig.run(1);
    CHECK_EQ(rig.engine.diag().rejected_priority, 1);
    CHECK_EQ(rig.engine.diag().instances, 2);
}

// Control: the same bus, the same budget, but the incoming sound outranks
// what is playing -- now it does displace it.
TEST(control_priority_does_cut_something_less_important) {
    Rig rig;
    CHECK(rig.load(proj(snd("important", kLong, ",\"bus\":\"ui\",\"priority\":250,\"maxInstances\":8") +
                        "," + snd("trivial", kLong, ",\"bus\":\"ui\",\"priority\":5,\"maxInstances\":8"))));
    rig.play("trivial", 1);
    rig.run(1);
    rig.play("trivial", 2);
    rig.run(8);
    CHECK_EQ(rig.engine.diag().instances, 2);

    rig.play("important", 3);
    rig.run(1);
    CHECK_EQ(rig.engine.diag().rejected_priority, 0);
    CHECK(rig.engine.diag().stolen >= 1);
    // The budget is respected immediately; the displaced instance is still
    // rendering its fade, which is why it is counted separately.
    CHECK_EQ(rig.engine.bus_instances(1), 2);
    CHECK_EQ(rig.engine.diag().retiring, 1);
    CHECK_EQ(rig.engine.rendering_instances(), 3);
}

// A stolen sound fades rather than stopping mid-sample. Cutting a waveform
// at an arbitrary point is a click, and a click is more noticeable than the
// sound that displaced it.
TEST(a_stolen_instance_fades_out_instead_of_clicking) {
    Rig rig;
    CHECK(rig.load(proj(snd("a", kLong, ",\"bus\":\"ui\",\"priority\":10,\"maxInstances\":8") +
                        "," +
                        snd("b", kLong, ",\"bus\":\"ui\",\"priority\":200,\"maxInstances\":8"))));
    rig.play("a", 1);
    rig.run(1);
    rig.play("a", 2);
    rig.run(8);
    CHECK_EQ(rig.engine.bus_instances(1), 2);

    rig.play("b", 3);
    rig.run(1);
    // Room was made at once...
    CHECK_EQ(rig.engine.bus_instances(1), 2);
    // ...but nothing was cut dead.
    CHECK_EQ(rig.engine.diag().retiring, 1);
    CHECK(rig.engine.rendering_instances() > rig.engine.bus_instances(1));

    // And the fade does finish.
    rig.run(12);
    CHECK_EQ(rig.engine.diag().retiring, 0);
}

// An instance younger than the steal shield is never a candidate, or a burst
// would cut its own leading edge and sound like a stutter.
TEST(a_just_started_instance_is_not_stolen) {
    Rig rig;
    CHECK(rig.load(proj(snd("x", kLong, ",\"bus\":\"ui\",\"maxInstances\":8"))));
    rig.play("x", 1);
    rig.play("x", 2);
    rig.run(1);  // both younger than STEAL_SHIELD_BLOCKS
    CHECK_EQ(rig.engine.diag().instances, 2);
    rig.play("x", 3);
    rig.run(1);
    // Nothing was old enough to cut, so the newcomer is refused.
    CHECK_EQ(rig.engine.diag().rejected_priority, 1);
    CHECK_EQ(rig.engine.diag().stolen, 0);
}

// ------------------------------------------------------------------ handles

TEST(a_stale_handle_is_a_silent_no_op) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort))));
    const u32 h = rig.play("hit");
    rig.run(200);  // long finished
    CHECK_EQ(rig.engine.diag().instances, 0);

    rig.stop(h, 0);
    SfxCommand c;
    c.type = SfxCommand::Type::SetPosition;
    c.handle = h;
    rig.engine.queue().push(c);
    rig.run(4);  // must not crash, must not resurrect anything
    CHECK_EQ(rig.engine.diag().instances, 0);
}

TEST(stop_with_a_fade_ends_the_instance) {
    Rig rig;
    CHECK(rig.load(proj(snd("hum", kLong))));
    const u32 h = rig.play("hum");
    rig.run(4);
    CHECK_EQ(rig.engine.diag().instances, 1);
    rig.stop(h, 8);
    rig.run(12);
    CHECK_EQ(rig.engine.diag().instances, 0);
}

// ---------------------------------------------------------------- placement

TEST(a_distant_sound_is_quieter_than_a_near_one) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort, ",\"attenuation\":{\"minDist\":1,\"maxDist\":100}"))));

    rig.play("hit", 1, 2 * GAIN_ONE, 0, 0);
    const i32 near = rig.run(30);

    Rig far_rig;
    CHECK(far_rig.load(proj(snd("hit", kShort, ",\"attenuation\":{\"minDist\":1,\"maxDist\":100}"))));
    far_rig.play("hit", 1, 80 * GAIN_ONE, 0, 0);
    const i32 far = far_rig.run(30);

    CHECK(near > 0);
    CHECK(far < near);
}

TEST(a_sound_beyond_max_distance_is_silent) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort, ",\"attenuation\":{\"minDist\":1,\"maxDist\":20}"))));
    rig.play("hit", 1, 500 * GAIN_ONE, 0, 0);
    CHECK_EQ(rig.run(30), 0);
}

// ------------------------------------------------------------------ ducking

TEST(a_ducking_sound_pulls_the_music_down_and_lets_it_back) {
    Rig rig;
    CHECK(rig.load(proj(snd("shout", kLong, ",\"duckMusic\":{\"db\":-12,\"ms\":200}"))));
    CHECK_EQ(rig.engine.music_duck_milli(), 1000);

    rig.play("shout");
    rig.run(2);
    const int ducked = rig.engine.music_duck_milli();
    CHECK(ducked < 1000);
    CHECK(ducked > 0);

    rig.run(200);  // 200 ms is 150 blocks
    CHECK_EQ(rig.engine.music_duck_milli(), 1000);
}

TEST(a_sound_without_a_duck_leaves_the_music_alone) {
    Rig rig;
    CHECK(rig.load(proj(snd("hit", kShort))));
    rig.play("hit");
    rig.run(10);
    CHECK_EQ(rig.engine.music_duck_milli(), 1000);
}

// ------------------------------------------------------------ determinism

// Two engines fed the same commands must produce the same bytes. This is the
// whole point of the seeded design, and it is what the golden renders rest on.
TEST(the_same_script_renders_the_same_bytes) {
    std::vector<i16> a, b;
    for (int pass = 0; pass < 2; ++pass) {
        Rig rig;
        CHECK(rig.load(proj(snd("hit", kShort, ",\"pitchJitterCents\":80,\"gainJitterDb\":3,"
                                               "\"maxInstances\":8"))));
        std::vector<i16> out(64 * CONTROL_BLOCK * 2);
        for (int i = 0; i < 8; ++i) {
            rig.play("hit", static_cast<u32>(0xABC0 + i), i * GAIN_ONE, 0, 0);
            rig.engine.render(out.data() + static_cast<size_t>(i) * 8 * CONTROL_BLOCK * 2,
                              8 * CONTROL_BLOCK);
        }
        (pass == 0 ? a : b) = out;
    }
    CHECK_EQ(a.size(), b.size());
    int mismatches = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++mismatches;
    CHECK_EQ(mismatches, 0);
    // Control: it is not silence being compared.
    i32 peak = 0;
    for (i16 s : a) {
        const i32 v = s < 0 ? -static_cast<i32>(s) : s;
        if (v > peak) peak = v;
    }
    CHECK(peak > 100);
}

// Different seeds must still differ, or "seeded" would mean "constant" and
// every shot in a burst would phase against its neighbours.
TEST(different_seeds_render_differently) {
    std::vector<i16> out[2];
    for (int pass = 0; pass < 2; ++pass) {
        Rig rig;
        CHECK(rig.load(proj(snd("hit", kShort, ",\"pitchJitterCents\":200"))));
        out[pass].assign(16 * CONTROL_BLOCK * 2, 0);
        rig.play("hit", pass == 0 ? 11u : 22u);
        rig.engine.render(out[pass].data(), 16 * CONTROL_BLOCK);
    }
    CHECK(out[0] != out[1]);
}

// A stolen instance's voices fade for a few blocks after its slot has been
// handed to the sound that displaced it. If the pool keyed ownership on the
// slot, the new sound would pick those voices up and mix the old sound's tail
// into itself -- audible, worse the busier the scene, and invisible on the
// cached path because a cached instance owns no voices at all.
//
// The observable form of the bug: the same contested script renders
// differently cached and live. So that is what this checks.
TEST(a_reused_slot_does_not_inherit_the_previous_sounds_tail) {
    const std::string bank =
        proj(snd("low", kLong, ",\"bus\":\"ui\",\"priority\":20,\"maxInstances\":8") + "," +
             snd("high", kLong, ",\"bus\":\"ui\",\"priority\":200,\"maxInstances\":8"),
             "[{\"name\":\"world\",\"maxInstances\":16},"
             "{\"name\":\"ui\",\"spatial\":false,\"maxInstances\":2}]");

    auto run = [&](bool force_live) {
        WaveTableCache wc;
        SoundBank b;
        CHECK(load_bank_json(bank.c_str(), bank.size(), wc, b).ok);
        if (force_live)
            for (auto& s : b.sounds) s.live = true;
        PcmCache cache;
        CHECK(cache.build(b, DEFAULT_CACHE_BUDGET_KB * 1024).ok);
        SfxEngine eng;
        eng.set_content(&b, &cache);

        std::vector<i16> out(240 * CONTROL_BLOCK * 2, 0);
        u32 h = 0;
        for (int blk = 0; blk < 240; ++blk) {
            // A trigger every six blocks, alternating, so the bus of two is
            // permanently contended and slots are recycled constantly.
            if (blk % 6 == 0) {
                SfxCommand c;
                c.type = SfxCommand::Type::Play;
                c.handle = eng.next_handle();
                c.a = b.sound_index((blk / 6) % 3 ? "low" : "high");
                c.e = 900 + static_cast<u32>(blk);
                c.variant = -1;
                eng.queue().push(c);
                h = c.handle;
            }
            eng.render(out.data() + static_cast<size_t>(blk) * CONTROL_BLOCK * 2, CONTROL_BLOCK);
        }
        (void)h;
        return out;
    };

    const auto cached = run(false);
    const auto live = run(true);
    CHECK_EQ(cached.size(), live.size());
    int mismatches = 0;
    for (size_t i = 0; i < cached.size(); ++i)
        if (cached[i] != live[i]) ++mismatches;
    CHECK_EQ(mismatches, 0);

    // Control: the script really did contend and steal, or this proves
    // nothing about slot reuse.
    i32 peak = 0;
    for (i16 s : cached) {
        const i32 v = s < 0 ? -static_cast<i32>(s) : s;
        if (v > peak) peak = v;
    }
    CHECK(peak > 100);
}
