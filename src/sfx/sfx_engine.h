#pragma once
//
// The sound engine: commands in, interleaved stereo int16 out.
//
// It is a separate engine instance from the music synthesiser, and that is a
// design decision rather than an accident of layout. midisynth's voice pool is
// flat and shared by construction, so a burst of forty impacts inside it would
// rank the music's sustaining pads as the best thing to steal and cut them.
// Two engines make that impossible rather than merely unlikely.
//
// What it costs: there is no shared master bus, so the host sums two already
// mastered streams (in float, never in int16 -- see the C API), and no shared
// reverb, so a gunshot cannot sit in the music's hall. Ducking crosses the gap
// as a number: music_duck_milli() is polled and handed to the music engine.
//
// THREADING. render() is the only call that belongs on the audio thread.
// Everything else either goes through the lock-free queue, or -- for the
// loaders -- must not run while render() is running. Same contract as
// midisynth's C API, for the same reason.
//
#include "config.h"
#include "instance.h"
#include "pcm_cache.h"
#include "sequencer/event_queue.h"
#include "sound_def.h"
#include "spatial.h"
#include "synth/effects/reverb.h"
#include "synth/limiter.h"
#include "synth/master_chain.h"
#include "voice_alloc.h"

namespace dsnd {

struct SfxCommand {
    enum class Type : u8 {
        SetListener,
        Play,
        Stop,
        StopBus,
        StopAll,
        SetPosition,
        SetGain,
        SetPitch,
        BusGain,
        MasterVolume,
        MasterParam,
        Dither,
    };

    // Absolute engine sample position. Anything at or before `now` applies at
    // the next control block, which is what every gameplay trigger wants.
    u64 when = 0;
    Type type = Type::Play;
    u32 handle = 0;
    i32 a = 0, b = 0, c = 0, d = 0;
    u32 e = 0;
    // Play only: which variant to use, or -1 to pick by weight from the seed.
    // A caller who names one is usually a tool auditioning a specific take,
    // or a golden script pinning what it renders.
    i32 variant = -1;
};

using SfxQueue = msynth::SpscQueue<SfxCommand, 10>;  // 1024 commands

// Why a play was refused. Every one of these is counted, because "the sound
// did not play" is otherwise the least debuggable complaint in a game.
struct Diagnostics {
    i32 instances = 0;      // counting against budgets
    i32 retiring = 0;       // fading out; still rendered, no longer counted
    i32 peak_instances = 0;
    i32 live_instances = 0;
    i32 cached_instances = 0;
    i32 synth_voices = 0;
    i32 peak_synth_voices = 0;

    i64 played = 0;
    i64 rejected_cooldown = 0;
    i64 rejected_max_instances = 0;
    i64 rejected_priority = 0;
    i64 rejected_bus_budget = 0;
    i64 rejected_no_voices = 0;
    i64 rejected_unknown = 0;
    i64 stolen = 0;

    u64 queue_dropped = 0;
    u32 queue_high_water = 0;
};

class SfxEngine {
  public:
    SfxEngine();

    // Control thread, NOT concurrent with render().
    void set_content(const SoundBank* bank, const PcmCache* cache);

    SfxQueue& queue() { return queue_; }
    u64 now() const { return sample_time_; }

    // Audio thread only. `frames` must be a multiple of CONTROL_BLOCK.
    void render(i16* out_interleaved, int frames);

    // 1000 = no duck. A pure function of how long ago each duck was
    // triggered -- the same discipline as midisynth's sidechain, which is
    // note-triggered rather than level-following precisely so that it is
    // reproducible. Poll once per callback and hand it to the music engine.
    int music_duck_milli() const;

    // Is this handle still sounding? A stale one is simply false -- handles
    // are never reused, so there is no chance of answering about a different
    // instance that happens to have landed in the same slot.
    bool alive(u32 handle) const { return find_instance(handle) >= 0; }

    // How many instances count against a bus's budget. Retiring ones do not:
    // they are still rendering their fade, but they have already made room.
    int bus_instances(u8 bus) const;

    // Everything still rendering, including fades. Always >= bus_instances.
    int rendering_instances() const;

    // Tracing only: the instance table as it stands.
    const Instance& instance(int i) const { return inst_[i]; }

    const Diagnostics& diag() const { return diag_; }
    msynth::MasterChain& master() { return master_; }
    msynth::OutputStage& output() { return out_; }

    // Exposed for the offline renderer, which needs to place a trigger at an
    // exact sample rather than "as soon as possible".
    u32 next_handle() { return ++handle_seq_; }

  private:
    void render_one_block(i64* mix_l, i64* mix_r);
    void apply(const SfxCommand& c);
    void do_play(const SfxCommand& c);
    int find_instance(u32 handle) const;
    int free_instance_slot() const;
    int steal_candidate(u8 bus, u8 priority) const;
    // Stop counting an instance and fade it out. It keeps rendering until the
    // fade ends -- see Instance::counted.
    void retire_instance(int idx, u32 fade_blocks);
    // Drop it immediately, tail and all. For unload and for an explicit stop
    // with no fade.
    void end_instance(int idx);

    const SoundBank* bank_ = nullptr;
    const PcmCache* cache_ = nullptr;

    SfxQueue queue_;
    u64 sample_time_ = 0;

    Instance inst_[MAX_SFX_INSTANCES];
    SfxVoicePool pool_;
    ChannelState channel_;
    Listener listener_;

    AirFilter air_[MAX_SFX_INSTANCES];

    // Per-sound admission state, indexed by sound id.
    std::vector<u64> last_trigger_;
    std::vector<u8> live_count_;

    i32 bus_gain_[MAX_BUSES];
    i32 bus_gain_target_[MAX_BUSES];
    i32 bus_gain_step_[MAX_BUSES];

    // Active duck requests: a gain and the block it expires.
    struct Duck {
        i32 gain_q16 = GAIN_ONE;
        u64 until_block = 0;
    };
    Duck ducks_[MAX_DUCK_REQUESTS];

    msynth::Reverb reverb_;
    // Chorus is what makes one synthesized voice sound like several -- the
    // "choir" of a monster chorus, the beating of a machine. Header-only in
    // midisynth, so it costs a member and no new translation unit.
    msynth::Chorus chorus_;
    msynth::DelayFx delay_;
    msynth::MasterChain master_;
    msynth::OutputStage out_;

    u32 handle_seq_ = 0;
    u16 gen_[MAX_SFX_INSTANCES] = {};
    u64 block_count_ = 0;
    Diagnostics diag_;
};

}  // namespace dsnd
