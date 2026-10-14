#include "sfx_engine.h"

#include "sfx_json.h"

namespace dsnd {
namespace {

using msynth::gmul;
using msynth::pan_gains;

i32 clampg(i64 v, i64 lo, i64 hi) {
    return static_cast<i32>(v < lo ? lo : (v > hi ? hi : v));
}

// Jitter in [-range, +range] from one draw. Symmetric, and zero range gives
// exactly zero rather than a rounding of it -- a sound with no jitter must be
// bit-identical to one whose jitter code never ran.
i32 jitter(msynth::XorShift32& rng, i32 range) {
    if (range <= 0) return 0;
    const i64 r = static_cast<i64>(static_cast<i32>(rng.next()));  // -2^31 .. 2^31-1
    return static_cast<i32>((r * range) / 2147483648LL);
}

}  // namespace

SfxEngine::SfxEngine() {
    // Master headroom.
    //
    // midisynth defaults to GAIN_ONE/8 because a piece of music can have
    // thirty-two notes sounding at once and they sum. Sound effects do not
    // behave that way: the bus budgets cap how many can overlap, distance
    // attenuation pulls most of them down, and a typical moment has a handful
    // rather than dozens. Carrying 18 dB of headroom for polyphony that the
    // limiter is there to catch anyway just makes every bank quiet and pushes
    // authors to compensate in content, where it is much harder to undo.
    //
    // GAIN_ONE/2 leaves 6 dB, which a full bus can still exceed -- and that is
    // what the master chain's limiter is for, if a bank turns it on.
    out_.master_gain = GAIN_ONE / 2;
    channel_ = neutral_channel();
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i) {
        inst_[i].reset();
        inst_[i].slot = static_cast<u16>(i);
    }
    for (int i = 0; i < MAX_BUSES; ++i)
        bus_gain_[i] = bus_gain_target_[i] = GAIN_ONE, bus_gain_step_[i] = 0;
}

void SfxEngine::set_content(const SoundBank* bank, const PcmCache* cache) {
    // Every instance holds pointers into the outgoing bank's patches and PCM.
    // Stopping them is not optional.
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i) inst_[i].reset();
    pool_.release_all();
    pool_.reap();

    bank_ = bank;
    cache_ = cache;
    if (bank_) {
        last_trigger_.assign(bank_->sounds.size(), 0);
        live_count_.assign(bank_->sounds.size(), 0);
        reverb_.set_params(bank_->reverb);
        reverb_.clear();
        chorus_.set_params(bank_->chorus);
        chorus_.clear();
        delay_.set_params(bank_->delay);
        delay_.clear();
        master_.set_params(bank_->master);
        for (size_t i = 0; i < bank_->buses.size() && i < MAX_BUSES; ++i)
            bus_gain_[i] = bus_gain_target_[i] = bank_->buses[i].gain_q16;
    } else {
        last_trigger_.clear();
        live_count_.clear();
    }
    diag_ = Diagnostics{};
}

int SfxEngine::find_instance(u32 handle) const {
    if (handle == 0) return -1;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
        if (inst_[i].active && inst_[i].handle == handle) return i;
    return -1;
}

int SfxEngine::bus_instances(u8 bus) const {
    int n = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
        if (inst_[i].active && inst_[i].counted && inst_[i].bus == bus) ++n;
    return n;
}

int SfxEngine::rendering_instances() const {
    int n = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
        if (inst_[i].active) ++n;
    return n;
}

int SfxEngine::free_instance_slot() const {
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
        if (!inst_[i].active) return i;
    return -1;
}

// Rank the instances of one bus and return the best thing to cut, or -1 if
// nothing may be cut.
//
// Releasing first, then lowest priority, then quietest, then oldest. The
// audibility term is the spatial gain the instance currently has, which is
// the right measure here and is already computed -- midisynth ranks by
// envelope level instead, which for a sound effect says more about the
// patch's shape than about whether the player can hear it.
int SfxEngine::steal_candidate(u8 bus, u8 priority) const {
    int best = -1;
    u64 best_key = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i) {
        const Instance& in = inst_[i];
        if (!in.active || !in.counted || in.bus != bus) continue;
        // Too young to steal. Without this a burst cuts its own front.
        if (in.age_blocks < STEAL_SHIELD_BLOCKS) continue;

        const u64 cls = in.releasing ? 0u : 1u;
        const u64 aud = static_cast<u64>(clampg(in.cur_gain_q16, 0, 0xFFFFF));
        const u64 age = 0x3FFFFFu - (in.age_blocks > 0x3FFFFFu ? 0x3FFFFFu : in.age_blocks);
        const u64 key = (cls << 62) | (static_cast<u64>(in.priority) << 54) | (aud << 22) | age;
        if (best < 0 || key < best_key) {
            best = i;
            best_key = key;
        }
    }
    if (best < 0) return -1;
    // A more important sound is never cut for a less important one. The new
    // play is refused instead, which is what makes `priority` a promise
    // rather than a hint.
    if (inst_[best].priority > priority) return -1;
    return best;
}

void SfxEngine::retire_instance(int idx, u32 fade_blocks) {
    Instance& in = inst_[idx];
    if (!in.active) return;
    if (in.counted) {
        const i32 s = in.sound;
        if (s >= 0 && static_cast<size_t>(s) < live_count_.size() && live_count_[s] > 0)
            --live_count_[s];
        in.counted = false;
    }
    // The voices are NOT fast-released here, and that is load-bearing.
    //
    // Fading them as well as fading the instance would give a live instance
    // two fades and a cached one only the instance's -- so a stolen sound
    // would render differently depending on whether it happened to be cached,
    // and the cached-equals-live property would be false exactly when the
    // engine is busiest. One fade, applied to the instance's lane, is the same
    // fade whichever way the lane was filled.
    //
    // The cost is that the synth pool frees a few blocks after the budget
    // does, which at three blocks of fade is not worth a second mechanism.
    // end_instance() releases them when the fade finishes.
    in.releasing = true;
    if (fade_blocks == 0) fade_blocks = 1;
    // A fade already running is not restarted -- being stolen twice should
    // not make a sound last longer.
    if (in.fade_total == 0) {
        in.fade_left = in.fade_total = fade_blocks;
    }
}

void SfxEngine::end_instance(int idx) {
    Instance& in = inst_[idx];
    pool_.release_instance(in.owner);
    if (in.counted) {
        const i32 s = in.sound;
        if (s >= 0 && static_cast<size_t>(s) < live_count_.size() && live_count_[s] > 0)
            --live_count_[s];
    }
    const u16 slot = in.slot;
    in.reset();
    in.slot = slot;
    air_[idx].reset();
}

void SfxEngine::do_play(const SfxCommand& c) {
    if (!bank_) {
        ++diag_.rejected_unknown;
        return;
    }
    const i32 sid = c.a;
    if (sid < 0 || static_cast<size_t>(sid) >= bank_->sounds.size()) {
        ++diag_.rejected_unknown;
        return;
    }
    const SoundDef& snd = bank_->sounds[static_cast<size_t>(sid)];

    // ---- tier (a): per-sound admission, before anything is allocated ----
    //
    // Two integer comparisons, and they do most of the real work: eight
    // monsters firing the same pistol in one tick is one trigger and seven
    // rejections, none of which touches the pool.
    if (snd.cooldown_blocks > 0 && last_trigger_[sid] != 0) {
        const u64 since = block_count_ - last_trigger_[sid];
        if (since < snd.cooldown_blocks) {
            ++diag_.rejected_cooldown;
            return;
        }
    }
    if (live_count_[sid] >= snd.max_instances) {
        if (snd.on_limit == OnLimit::Reject) {
            ++diag_.rejected_max_instances;
            return;
        }
        // StealOldest: cut this sound's own oldest, never anyone else's.
        int oldest = -1;
        u32 oldest_age = 0;
        for (int i = 0; i < MAX_SFX_INSTANCES; ++i) {
            if (!inst_[i].active || !inst_[i].counted || inst_[i].sound != sid) continue;
            if (oldest < 0 || inst_[i].age_blocks > oldest_age) {
                oldest = i;
                oldest_age = inst_[i].age_blocks;
            }
        }
        if (oldest >= 0) {
            retire_instance(oldest, STEAL_FADE_BLOCKS);
            ++diag_.stolen;
        }
    }

    // ---- tier (b): the bus budget ----
    const BusDef& bus = bank_->buses[snd.bus];
    int on_bus = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
        if (inst_[i].active && inst_[i].counted && inst_[i].bus == snd.bus) ++on_bus;
    if (on_bus >= bus.max_instances) {
        const int victim = steal_candidate(snd.bus, snd.priority);
        if (victim < 0) {
            ++diag_.rejected_priority;
            return;
        }
        retire_instance(victim, STEAL_FADE_BLOCKS);
        ++diag_.stolen;
    }

    int slot = free_instance_slot();
    if (slot < 0) {
        ++diag_.rejected_bus_budget;
        return;
    }

    Instance& in = inst_[slot];
    const u16 keep = in.slot;
    in.reset();
    in.slot = keep;
    // A fresh owner, so any voices still fading from the instance that used
    // this slot before are not claimed by this one.
    in.owner = static_cast<u16>((keep & 0x3F) | (static_cast<u16>(++gen_[keep] & 0x3FF) << 6));
    in.handle = c.handle;
    in.active = true;
    in.counted = true;
    in.sound = sid;
    in.bus = snd.bus;
    in.priority = snd.priority;
    in.seed = c.e;
    in.spatial = bus.spatial;
    in.x_q16 = c.b;
    in.y_q16 = c.c;
    in.z_q16 = c.d;
    in.pan_q16 = bus.spatial ? 0 : c.b;

    // The seed drives every random choice, in a fixed order, so the same
    // trigger from the same game event is the same sound on every machine.
    msynth::XorShift32 rng(c.e ? c.e : 0x9E3779B9u);

    // Variant: the caller's, or weighted by the seed.
    //
    // The seed is drawn from either way, so that naming a variant does not
    // shift the jitter that follows it. Two triggers with the same seed get
    // the same pitch and gain whether or not one of them picked its variant
    // by hand -- otherwise a tool auditioning variant 2 would hear a
    // different pitch from the game playing variant 2.
    i32 want_variant = c.variant;
    if (snd.variant_count > 0) {
        const u32 raw = rng.next();
        if (want_variant >= 0) {
            if (want_variant >= static_cast<i32>(snd.variant_count)) want_variant = 0;
        } else {
        u32 total = 0;
        for (u16 v = 0; v < snd.variant_count; ++v)
            total += bank_->variants[snd.variant_first + v].weight;
        if (total == 0) total = 1;
        u32 pick = raw % total;
        want_variant = 0;
        for (u16 v = 0; v < snd.variant_count; ++v) {
            const u32 w = bank_->variants[snd.variant_first + v].weight;
            if (pick < w) {
                want_variant = v;
                break;
            }
            pick -= w;
        }
        }
    }
    in.variant = static_cast<u16>(want_variant < 0 ? 0 : want_variant);

    const i32 pitch_cents = jitter(rng, snd.pitch_jitter_cents_q16);
    const i32 gain_db = jitter(rng, snd.gain_jitter_db_q16);

    // Caller gain x sound gain x jitter.
    i64 g = static_cast<i64>(snd.gain_q16);
    g = (g * db_q16_to_gain(gain_db)) / GAIN_ONE;
    in.gain_q16 = clampg(g, 0, 8 * GAIN_ONE);

    const Variant& var = bank_->variants[snd.variant_first + in.variant];
    in.live = snd.live || var.cache_index < 0;

    // A sound that only fell back to live must reproduce the cache exactly,
    // so it synthesizes from the cache's canonical seed rather than the
    // caller's. A sound the author marked live keeps the caller's, because
    // varying per trigger is the reason to mark it.
    if (in.live && !snd.live_authored) in.seed = mix_seed(0x5EEDu, in.variant, 0, 0);

    if (!in.live) {
        const CacheEntry& e = cache_->entry(static_cast<size_t>(var.cache_index));
        in.sample.pcm = e.data();
        in.sample.frames = e.frames;
        in.sample.stereo = e.stereo;
        in.sample.pos_q32 = 0;
        in.sample.looping = snd.loop;
        in.sample.rate_q32 = rate_from_cents(pitch_cents);
    }
    // A live sound has nothing started yet: its gesture notes fire from
    // render_one_block, and each of them needs this offset.
    in.pitch_cents_q16 = pitch_cents;
    in.cur_pan_q16 = 0;
    in.cur_gain_q16 = 0;
    in.placed = false;

    last_trigger_[sid] = block_count_ == 0 ? 1 : block_count_;
    ++live_count_[sid];
    ++diag_.played;

    // A duck request, if this sound asks for one.
    if (snd.duck_gain_q16 < GAIN_ONE && snd.duck_blocks > 0) {
        int worst = 0;
        for (int i = 1; i < MAX_DUCK_REQUESTS; ++i)
            if (ducks_[i].until_block < ducks_[worst].until_block) worst = i;
        ducks_[worst].gain_q16 = snd.duck_gain_q16;
        ducks_[worst].until_block = block_count_ + snd.duck_blocks;
    }
}

void SfxEngine::apply(const SfxCommand& c) {
    switch (c.type) {
        case SfxCommand::Type::SetListener:
            listener_.x_q16 = c.a;
            listener_.y_q16 = c.b;
            listener_.z_q16 = c.c;
            listener_.yaw_brad = c.e;
            break;
        case SfxCommand::Type::Play:
            do_play(c);
            break;
        case SfxCommand::Type::Stop: {
            const int i = find_instance(c.handle);
            if (i < 0) break;  // a stale handle is a no-op, never an error
            const u32 blocks = static_cast<u32>(c.a);
            if (blocks == 0) end_instance(i);
            else retire_instance(i, blocks);
            break;
        }
        case SfxCommand::Type::StopBus:
            for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
                if (inst_[i].active && inst_[i].bus == static_cast<u8>(c.a))
                    retire_instance(i, static_cast<u32>(c.b > 0 ? c.b : 1));
            break;
        case SfxCommand::Type::StopAll:
            for (int i = 0; i < MAX_SFX_INSTANCES; ++i)
                if (inst_[i].active) retire_instance(i, static_cast<u32>(c.a > 0 ? c.a : 1));
            break;
        case SfxCommand::Type::SetPosition: {
            const int i = find_instance(c.handle);
            if (i < 0) break;
            inst_[i].x_q16 = c.a;
            inst_[i].y_q16 = c.b;
            inst_[i].z_q16 = c.c;
            break;
        }
        case SfxCommand::Type::SetGain: {
            const int i = find_instance(c.handle);
            if (i < 0) break;
            inst_[i].gain_q16 = clampg(c.a, 0, 8 * GAIN_ONE);
            break;
        }
        case SfxCommand::Type::SetPitch: {
            const int i = find_instance(c.handle);
            if (i < 0 || inst_[i].live) break;  // live pitch is set at note-on
            inst_[i].sample.rate_q32 = rate_from_cents(c.a);
            break;
        }
        case SfxCommand::Type::BusGain: {
            const int b = c.a;
            if (b < 0 || b >= MAX_BUSES) break;
            bus_gain_target_[b] = clampg(c.b, 0, 8 * GAIN_ONE);
            const i32 blocks = c.c > 0 ? c.c : 1;
            bus_gain_step_[b] = (bus_gain_target_[b] - bus_gain_[b]) / blocks;
            if (bus_gain_step_[b] == 0) bus_gain_[b] = bus_gain_target_[b];
            break;
        }
        case SfxCommand::Type::MasterVolume:
            out_.master_gain = clampg(c.a, 0, GAIN_ONE);
            break;
        case SfxCommand::Type::MasterParam: {
            msynth::MasterParams p = master_.params();
            if (c.a >= 0 && c.a < static_cast<i32>(msynth::MasterParamId::COUNT)) {
                p.set(static_cast<msynth::MasterParamId>(c.a), c.b);
                master_.set_params(p);
            }
            break;
        }
        case SfxCommand::Type::Dither:
            out_.dither = c.a != 0;
            break;
    }
}

int SfxEngine::music_duck_milli() const {
    i32 g = GAIN_ONE;
    for (int i = 0; i < MAX_DUCK_REQUESTS; ++i)
        if (ducks_[i].until_block > block_count_ && ducks_[i].gain_q16 < g)
            g = ducks_[i].gain_q16;
    return static_cast<int>((static_cast<i64>(g) * 1000) / GAIN_ONE);
}

void SfxEngine::render_one_block(i64* mix_l, i64* mix_r) {
    // ---- commands due now ----
    SfxCommand c;
    while (queue_.peek(c) && c.when <= sample_time_) {
        queue_.pop();
        apply(c);
    }

    for (int i = 0; i < CONTROL_BLOCK; ++i) mix_l[i] = mix_r[i] = 0;
    i64 rev_l[CONTROL_BLOCK] = {}, rev_r[CONTROL_BLOCK] = {};
    i64 cho_l[CONTROL_BLOCK] = {}, cho_r[CONTROL_BLOCK] = {};
    i64 dly_l[CONTROL_BLOCK] = {}, dly_r[CONTROL_BLOCK] = {};
    bool rev_used = false, cho_used = false, dly_used = false;

    // ---- bus gain ramps ----
    for (int b = 0; b < MAX_BUSES; ++b) {
        if (bus_gain_[b] == bus_gain_target_[b]) continue;
        bus_gain_[b] += bus_gain_step_[b];
        if ((bus_gain_step_[b] > 0 && bus_gain_[b] > bus_gain_target_[b]) ||
            (bus_gain_step_[b] < 0 && bus_gain_[b] < bus_gain_target_[b]))
            bus_gain_[b] = bus_gain_target_[b];
    }

    int live_n = 0, cached_n = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i) {
        Instance& in = inst_[i];
        if (!in.active) continue;

        const SoundDef& snd = bank_->sounds[static_cast<size_t>(in.sound)];
        const BusDef& bus = bank_->buses[in.bus];

        if (in.live) {
            advance_gesture(in, *bank_, pool_, channel_, in.pitch_cents_q16);
            ++live_n;
        } else {
            ++cached_n;
        }
        const bool alive = render_sources(in, *bank_, pool_, channel_);

        bool lane_zero = true;
        for (int s = 0; s < CONTROL_BLOCK && lane_zero; ++s)
            if (in.lane_l[s] != 0 || in.lane_r[s] != 0) lane_zero = false;
        if (lane_zero) ++in.silent_blocks;
        else in.silent_blocks = 0;

        // ---- placement ----
        Placement p;
        if (in.spatial) {
            p = place(listener_, snd.atten, in.x_q16, in.y_q16, in.z_q16);
        } else {
            p.gain_q16 = GAIN_ONE;
            p.pan_q16 = in.pan_q16;
            p.lpf_hz_q16 = 0;
        }

        i64 target_gain = (static_cast<i64>(p.gain_q16) * in.gain_q16) / GAIN_ONE;
        target_gain = (target_gain * bus_gain_[in.bus]) / GAIN_ONE;
        if (in.fade_total > 0) {
            target_gain = (target_gain * in.fade_left) / in.fade_total;
            if (in.fade_left > 0) --in.fade_left;
        }

        // Seed at the target on the first block rather than ramping up to it.
        // A voice already ramps its own output across its first 64 samples,
        // and that ramp is inside the cached PCM: ramping again here would
        // make the cached path quieter than the live one for exactly one
        // block, which is the kind of difference nobody finds by listening.
        if (!in.placed) {
            in.cur_gain_q16 = static_cast<i32>(target_gain);
            in.cur_pan_q16 = p.pan_q16;
            in.placed = true;
            air_[i].reset();
        }

        if (p.lpf_hz_q16 > 0) {
            air_[i].set_hz(p.lpf_hz_q16);
            air_[i].process(in.lane_l, in.lane_r);
        }

        const i32 g0 = in.cur_gain_q16;
        const i32 g1 = static_cast<i32>(target_gain);
        const i32 pan0 = in.cur_pan_q16;
        const i32 pan1 = p.pan_q16;

        gain_t gl0, gr0, gl1, gr1;
        pan_gains(pan0, gl0, gr0);
        pan_gains(pan1, gl1, gr1);

        const i32 send = bus.send_reverb_q16;
        const i32 send_cho = bus.send_chorus_q16;
        const i32 send_dly = bus.send_delay_q16;
        for (int s = 0; s < CONTROL_BLOCK; ++s) {
            // Linear across the block: 64 samples is 1.3 ms, short enough that
            // a straight line is inaudible and cheap enough to do per sample.
            const i64 t = s;
            const i64 g = g0 + ((static_cast<i64>(g1) - g0) * t) / CONTROL_BLOCK;
            const i64 cl = gl0 + ((static_cast<i64>(gl1) - gl0) * t) / CONTROL_BLOCK;
            const i64 cr = gr0 + ((static_cast<i64>(gr1) - gr0) * t) / CONTROL_BLOCK;

            i64 sl, sr;
            if (in.spatial) {
                // A point source is mono: its own stereo image would fight
                // the panner. The same sum the cache applies when it stores a
                // spatial sound, so the two paths stay identical.
                const i64 m = msynth::asr(in.lane_l[s] + in.lane_r[s], 1);
                sl = (m * cl) / GAIN_ONE;
                sr = (m * cr) / GAIN_ONE;
            } else {
                sl = (in.lane_l[s] * cl) / GAIN_ONE;
                sr = (in.lane_r[s] * cr) / GAIN_ONE;
            }
            sl = (sl * g) / GAIN_ONE;
            sr = (sr * g) / GAIN_ONE;
            mix_l[s] += sl;
            mix_r[s] += sr;
            if (send > 0) {
                rev_l[s] += (sl * send) / GAIN_ONE;
                rev_r[s] += (sr * send) / GAIN_ONE;
            }
            if (send_cho > 0) {
                cho_l[s] += (sl * send_cho) / GAIN_ONE;
                cho_r[s] += (sr * send_cho) / GAIN_ONE;
            }
            if (send_dly > 0) {
                dly_l[s] += (sl * send_dly) / GAIN_ONE;
                dly_r[s] += (sr * send_dly) / GAIN_ONE;
            }
        }
        if (send > 0) rev_used = true;
        if (send_cho > 0) cho_used = true;
        if (send_dly > 0) dly_used = true;

        in.cur_gain_q16 = g1;
        in.cur_pan_q16 = pan1;
        ++in.age_blocks;

        // A gesture with a gap in it is silent between its notes, so an
        // instance is only finished once it has nothing left to fire. A
        // cached instance runs no gesture, and its whole gesture is inside
        // the samples it is playing back.
        const Variant& var = bank_->variants[snd.variant_first + in.variant];
        const bool notes_done = !in.live || in.next_note >= var.gesture.size();
        const bool finished = in.silent_blocks >= END_SILENT_BLOCKS && notes_done;
        const bool faded_out = in.fade_total > 0 && in.fade_left == 0;
        if (finished || faded_out) end_instance(i);
        (void)alive;
    }

    pool_.reap();

    // Each effect adds into the dry mix, exactly as reverb always has. The
    // guard matters for more than speed: an effect that is never fed must not
    // run at all, or its internal delay lines would advance and the render
    // would depend on how long the engine had been alive.
    if (rev_used || bank_->reverb.wet_gain > 0)
        reverb_.process_block(rev_l, rev_r, mix_l, mix_r, CONTROL_BLOCK);
    if (cho_used || bank_->chorus.wet_gain > 0)
        chorus_.process_block(cho_l, cho_r, mix_l, mix_r, CONTROL_BLOCK);
    if (dly_used || bank_->delay.wet_gain > 0)
        delay_.process_block(dly_l, dly_r, mix_l, mix_r, CONTROL_BLOCK);

    diag_.instances = 0;
    diag_.retiring = 0;
    for (int i = 0; i < MAX_SFX_INSTANCES; ++i) {
        if (!inst_[i].active) continue;
        if (inst_[i].counted) ++diag_.instances;
        else ++diag_.retiring;
    }
    if (diag_.instances > diag_.peak_instances) diag_.peak_instances = diag_.instances;
    diag_.live_instances = live_n;
    diag_.cached_instances = cached_n;
    diag_.synth_voices = pool_.active_count();
    diag_.peak_synth_voices = pool_.peak_active();
    diag_.queue_dropped = queue_.dropped();
    diag_.queue_high_water = queue_.high_water();

    ++block_count_;
}

void SfxEngine::render(i16* out_interleaved, int frames) {
    int done = 0;
    for (; done + CONTROL_BLOCK <= frames; done += CONTROL_BLOCK) {
        i64 mix_l[CONTROL_BLOCK], mix_r[CONTROL_BLOCK];
        if (!bank_) {
            for (int i = 0; i < CONTROL_BLOCK; ++i) mix_l[i] = mix_r[i] = 0;
            // Still drain the queue, so commands sent before a bank is loaded
            // do not pile up and then all fire at once when one arrives.
            SfxCommand c;
            while (queue_.peek(c) && c.when <= sample_time_) queue_.pop();
        } else {
            render_one_block(mix_l, mix_r);
        }

        if (master_.enabled()) {
            i32 ml[CONTROL_BLOCK], mr[CONTROL_BLOCK];
            for (int i = 0; i < CONTROL_BLOCK; ++i) {
                ml[i] = static_cast<i32>(out_.pre_gain(mix_l[i]));
                mr[i] = static_cast<i32>(out_.pre_gain(mix_r[i]));
            }
            master_.process_block(ml, mr, CONTROL_BLOCK);
            for (int i = 0; i < CONTROL_BLOCK; ++i) {
                out_interleaved[(done + i) * 2] = out_.finish(ml[i], 0);
                out_interleaved[(done + i) * 2 + 1] = out_.finish(mr[i], 1);
            }
        } else {
            for (int i = 0; i < CONTROL_BLOCK; ++i) {
                out_interleaved[(done + i) * 2] = out_.convert(mix_l[i], 0);
                out_interleaved[(done + i) * 2 + 1] = out_.convert(mix_r[i], 1);
            }
        }
        out_.advance_duck_block();
        sample_time_ += CONTROL_BLOCK;
    }
    // A partial trailing block is not rendered. The contract is a multiple of
    // CONTROL_BLOCK, and silently rendering part of one would put the engine
    // half a block out of step with its own clock forever after.
    for (int i = done; i < frames; ++i)
        out_interleaved[i * 2] = out_interleaved[i * 2 + 1] = 0;
}

}  // namespace dsnd
