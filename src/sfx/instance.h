#pragma once
//
// One playing sound.
//
// An instance is the unit of everything above the voice: of a handle, of a
// steal decision, of a position in the world, of a cooldown. That choice is
// deliberate and it is what makes stealing survivable -- a sound is its layers
// together, and cutting one layer out of an explosion removes the transient
// the player identifies the event by, which is worse than not playing it.
//
// It renders in one of two ways, and the point of the design is that a
// listener cannot tell which:
//
//   live    every gesture note starts a Voice per matching layer, through
//           midisynth's synthesis chain
//   cached  one SampleVoice reads the PCM that same chain produced once, at
//           load time
//
// Both fill the same per-instance stereo lane, and everything downstream --
// gain, pan, distance, sends -- is applied once, to the lane, by the
// spatialiser. Neither path applies any of it itself. That is what keeps the
// two bit-identical at unity rate (see sample_voice.h).
//
#include "config.h"
#include "resonator.h"
#include "sample_voice.h"
#include "sound_def.h"
#include "voice_alloc.h"

namespace dsnd {

// A gesture note that has been started and is waiting for its note-off.
struct PendingOff {
    u32 at_blocks = 0;  // instance age at which it fires
    u32 group = 0;      // which voice group it started
};

struct Instance {
    // ---- identity ----
    u32 handle = 0;  // 0 = free. Monotone, never reused.
    u16 slot = 0;  // index into the engine's instance table; never changes

    // The pool owner, and NOT the slot.
    //
    // A slot is reused as soon as its instance ends, but that instance's
    // voices linger for a few blocks while they fade. If ownership were the
    // slot, the next instance to land there would pick those voices up and
    // mix a stolen sound's tail into itself -- which is audible, is worse the
    // busier the scene gets, and does not happen on the cached path at all
    // because a cached instance owns no voices.
    //
    // So the owner is the slot plus a generation that bumps on every reuse:
    // six bits of slot, ten of generation. A lingering voice keeps the old
    // owner and is never claimed by the new instance. The handle would also
    // work but is a u32, and the pool's owner field is sixteen bits.
    u16 owner = 0;
    bool active = false;
    i32 sound = -1;
    u16 variant = 0;
    u8 bus = 0;
    u8 priority = 128;
    u32 seed = 0;

    // ---- lifetime ----
    u32 age_blocks = 0;
    bool releasing = false;
    bool live = false;

    // ---- gesture ----
    u8 next_note = 0;  // next gesture step to fire
    PendingOff offs[MAX_GESTURE_NOTES];
    u8 off_count = 0;
    u32 group_base = 0;

    // ---- sources ----
    SampleVoice sample;

    // ---- placement ----
    bool spatial = true;
    i32 x_q16 = 0, y_q16 = 0, z_q16 = 0;
    i32 pan_q16 = 0;     // direct pan for a non-spatial instance
    i32 gain_q16 = GAIN_ONE;  // the caller's gain x the sound's x jitter

    // Per-instance pitch offset from jitter or from the caller. A cached
    // instance folds it into the playback rate at start; a live one hands it
    // to each gesture note as it fires, which is why it has to be kept.
    i32 pitch_cents_q16 = 0;

    // The spatialiser's current values. They are seeded AT their target when
    // the instance starts, never ramped up from zero: a voice already ramps
    // its own output across its first 64 samples (voice.cpp:334) and that ramp
    // is inside the cached PCM, so a second ramp here would make the cached
    // path quieter than the live one for one block -- and the identity
    // property would be false in exactly the place nobody listens.
    i32 cur_gain_q16 = 0;
    i32 cur_pan_q16 = 0;
    bool placed = false;  // false until the first spatialise seeds the above

    // Fade applied on stop/steal, in blocks. 0 = not fading.
    u32 fade_left = 0, fade_total = 0;

    // Whether this instance still counts against its sound's cap and its
    // bus's budget. A retiring instance stops counting the moment it is
    // retired, so the sound that displaced it can start at once -- but it
    // keeps rendering until its fade finishes, because a sound cut mid-sample
    // clicks and a click is more noticeable than the sound it replaced.
    bool counted = false;

    // Consecutive blocks whose lane was exactly zero.
    //
    // What ends an instance is SILENCE, not its sources reporting themselves
    // finished, and the two are not the same: a cached entry stops when its
    // samples run out, while the live voices that produced those samples stay
    // formally active for a few blocks afterwards, emitting zeros. Ending on
    // "finished" would therefore free a cached instance's bus slot several
    // blocks before its live twin's -- inaudible in isolation, but enough to
    // change which later trigger the budget accepts, so a busy scene would
    // render differently depending on what happened to be cached.
    //
    // Both paths fill the same lane, so both see the same silence on the same
    // block.
    u8 silent_blocks = 0;

    // ---- the lane ----
    // Rung by the live path only. A cached instance is playing back samples
    // that were ALREADY rung when the cache was built, so resonating them
    // again would be applying the body twice.
    ResonatorState reson;
    bool reson_ready = false;

    i64 lane_l[CONTROL_BLOCK];
    i64 lane_r[CONTROL_BLOCK];

    void reset() {
        handle = 0;
        active = false;
        sound = -1;
        variant = 0;
        age_blocks = 0;
        releasing = false;
        counted = false;
        silent_blocks = 0;
        live = false;
        next_note = 0;
        off_count = 0;
        sample = SampleVoice{};
        pitch_cents_q16 = 0;
        group_base = 0;
        placed = false;
        fade_left = fade_total = 0;
        // A slot is reused, and a resonator holds two samples of history per
        // mode. Left behind, the previous sound's body would ring into the
        // next one -- the same class of bug as the voice-pool tail bleed,
        // which was audible and took a contended golden to find.
        reson_ready = false;
        reson.clear();
    }

    void clear_lane() {
        for (int i = 0; i < CONTROL_BLOCK; ++i) lane_l[i] = lane_r[i] = 0;
    }
};

// Start the gesture notes that come due this block, and fire the note-offs
// that have come due. Returns false if a note could not be started for want
// of voices -- the caller decides whether that ends the instance.
//
// Gesture notes fire on the first block whose age has reached their `at`, so
// a late block delays a note rather than losing it. That is the same rule the
// engine's weapon phases use for their action deadlines, and it is the right
// one here for the same reason: a dropped door creak is worse than a slightly
// late one.
bool advance_gesture(Instance& inst, const SoundBank& bank, SfxVoicePool& pool,
                     const ChannelState& ch, i32 extra_cents_q16);

// Render this instance's sources into its own lane. Does NOT apply gain, pan,
// distance or sends -- those belong to the spatialiser, once, downstream.
// Returns false once the instance has nothing left to sound.
bool render_sources(Instance& inst, const SoundBank& bank, SfxVoicePool& pool,
                    const ChannelState& ch);

}  // namespace dsnd
