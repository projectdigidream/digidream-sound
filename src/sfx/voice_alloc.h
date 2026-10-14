#pragma once
//
// Synth voices for `live` sounds: a flat pool over midisynth's Voice, with
// this project's own allocation rules.
//
// Why not midisynth's VoicePool: its members are private, it has no virtuals,
// and its whole surface is MIDI-shaped -- note_off_group(chan_slot, note),
// release_pedal, choke_group, alloc(outgoing_bank). The one thing we need to
// change is the ranking, and that is exactly the part that cannot be reached.
// Forking voice_pool.cpp would be forking DSP, which this project does not do.
//
// What is different here, and why:
//
//   * Allocation is ALL-OR-NOTHING. A sound is its layers together; half an
//     explosion is a worse artefact than none, because the missing half is
//     usually the transient the player identifies the event by.
//
//   * The unit of ownership is the INSTANCE, not the voice. Stealing kills an
//     instance whole. Ranking lives one level up, in the engine, where
//     priority and audibility are known; this pool only answers "are there n
//     free voices" and "let these go".
//
//   * The noise and LFO seeds are REPLACED after Voice::start. See
//     reseed_voice below -- this is the invariant the whole project rests on.
//
#include <memory>

#include "config.h"
#include "synth/channel_state.h"
#include "synth/voice.h"

namespace dsnd {

using msynth::ChannelState;
using msynth::LayerDef;
using msynth::Voice;
using msynth::VoiceBuses;

inline constexpr u16 NO_OWNER = 0xFFFF;

// A ChannelState that adds nothing of its own.
//
// Every one of these defaults is load-bearing, because a Voice reads channel
// state on every block and midisynth's defaults are tuned for General MIDI,
// not for us:
//
//   volume/expression 127  -- channel_gain_q16() is then exactly GAIN_ONE.
//                             The GM default of 100 would apply a ~-6 dB
//                             squared-law trim nobody asked for, and it would
//                             break the cached-equals-live identity.
//   sends 0                -- midisynth defaults send_reverb to 40 (the GM
//                             default). Voice::render_block dereferences the
//                             reverb bus pointers whenever a send is nonzero,
//                             and our per-instance lane has no send buses of
//                             its own: the send is applied once, downstream,
//                             by the bus. A nonzero default here would both
//                             double-send and read pointers we did not set.
//   pan 0, fade GAIN_ONE   -- panning is the spatialiser's job, once, at the
//                             end. Two pans in series are not a pan.
//   bend_raw 0             -- with no bend, bend_range_cents does not affect
//                             pitch at all; it only widens the wavetable mip
//                             chosen at note-on. The engine raises it for a
//                             live sound with wide pitch jitter so that the
//                             mip has headroom, and carries the jitter itself
//                             in fine_tune_cents_q16.
ChannelState neutral_channel();

// Derive a voice's PRNG seed from the caller's, never from allocation state.
u32 mix_seed(u32 seed, u32 variant, u32 layer, u32 unison);

// Overwrite the seeds Voice::start derived from its pool slot.
//
// THE PROBLEM. voice.cpp does this, under a comment saying output must not
// depend on allocation history:
//
//     const u32 seed = static_cast<u32>(pool_slot) * 2654435761u ^ midi_note;
//
// but `pool_slot` IS allocation history -- it is whichever voice happened to
// be free. Inside midisynth that is harmless, because a render is a closed
// system: the same song always allocates the same way. Here it is fatal. The
// same door, triggered twice from the same game event with the same seed,
// would sound different depending on what else was playing at the time, and a
// golden render would stop meaning anything.
//
// THE FIX. Voice::noise and LfoState are public, so the seeds can be replaced
// immediately after start(), before a single sample is rendered. LFOs go back
// through note_on() rather than having `rng` poked directly: note_on also
// draws one value into `held`, and skipping that draw leaves the sample-hold
// stream one step out of phase with a fresh voice -- a difference that only
// shows up on patches with a SampleHold LFO, which is the worst kind of bug
// to leave lying around.
void reseed_voice(Voice& v, const LayerDef& l, u32 seed, u64 blocks_elapsed = 0,
                  gain_t bpm_q16 = 120 << GAIN_SHIFT);

class SfxVoicePool {
  public:
    SfxVoicePool();

    // Reserve `n` voices for `owner`, or none at all. Returns the first, with
    // the rest contiguous in `out`. nullptr means the caller must not start.
    bool alloc_n(int n, u16 owner, Voice** out);

    // Ask every voice of an instance to fade out. They stay allocated until
    // they go idle, so the tail is heard.
    void release_instance(u16 owner);

    // Cut an instance immediately (steal). Same call, different intent --
    // fast_release is already a short ramp rather than a hard stop.
    void cut_instance(u16 owner) { release_instance(owner); }

    void release_all();

    // Hard reset: every voice inactive, every pointer dropped, immediately.
    //
    // release_all() is a fast FADE, which is what a running engine wants --
    // it keeps the tail and avoids a click. A pool being REUSED for an
    // unrelated offline render wants the opposite: its voices hold `layer`
    // and `patch` pointers into the bank that produced them, and that bank is
    // gone. Fading them would render through freed memory. There is no
    // continuity to preserve between two independent renders, so there is
    // nothing to fade.
    void reset();

    // Drop ownership of voices that have gone idle. Call once per block.
    void reap();

    int free_count() const;
    int active_count() const;
    int peak_active() const { return peak_; }

    Voice& voice(int i) { return voices_[i]; }
    u16 owner(int i) const { return owner_[i]; }
    static constexpr int capacity() { return MAX_SFX_VOICES; }

  private:
    // Heap, not inline: a Voice is ~17 KB and a stack-allocated pool would be
    // 820 KB of automatic storage. midisynth moved its own pool to the heap
    // for exactly this reason, after test fixtures started segfaulting.
    std::unique_ptr<Voice[]> voices_;
    u16 owner_[MAX_SFX_VOICES];

    // "This voice has been seen sounding." A voice is idle both before it
    // starts and after it finishes, and reap() must tell those apart: without
    // this, a voice reserved this block but not yet started would be handed
    // straight back, and two instances would render through it at once.
    bool armed_[MAX_SFX_VOICES];
    int peak_ = 0;
};

}  // namespace dsnd
