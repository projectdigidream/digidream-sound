#pragma once
//
// The runtime shape of a sound bank: what `.sfxproj` and `.sfxb` both resolve
// to before anything is played.
//
// Everything here is already in its final fixed-point form -- no milliseconds,
// no decibels, no floats. The parsers do that conversion once, on the control
// thread, so the audio thread never divides by 1000 or looks anything up in a
// string. A field's unit is in its name.
//
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "resonator.h"
#include "synth/effects/chorus.h"
#include "synth/effects/delay_fx.h"
#include "synth/effects/reverb.h"
#include "synth/master_chain.h"
#include "synth/patch_def.h"
#include "synth/tables.h"

namespace dsnd {

using msynth::CurveId;
using msynth::Patch;

// ---------------------------------------------------------------- attenuation

// How a source loses gain with distance, and how wide it pans.
//
// The defaults are Doom's own: S_CLOSE_DIST is 160 map units and
// S_CLIPPING_DIST is 1200, which at the engine's 1/48 scale are 3.33 and 25.0
// world units. A converted map therefore sounds the way its author placed it
// without anyone tuning a number.
struct Attenuation {
    i32 min_dist_q16 = 218453;   // 3.333 world units: full gain within this
    i32 max_dist_q16 = 1638400;  // 25.0 world units: silent beyond
    CurveId curve = CurveId::Linear;
    i32 spread_q16 = GAIN_ONE;  // scales pan width; 0 = always centre
    i32 rear_gain_q16 = GAIN_ONE;  // gain for a source directly behind
    i32 air_lpf_hz_q16 = 0;        // one-pole corner at max_dist; 0 = off
};

// ---------------------------------------------------------------- the gesture

// One note event inside a variant. Most variants have exactly one of these.
//
// Reach for a second only when the pitches differ: a Patch already carries
// four layers with their own start delays, round-robin groups and choke
// groups, so a shotgun's click + boom + tail is three LAYERS on one note --
// one voice group, one steal decision, the engine's own choke working. A
// gesture is for a door's thunk and its creak a beat later, at different
// pitches, which layers cannot express.
struct GestureNote {
    u32 at_blocks = 0;   // from instance start
    u32 dur_blocks = 0;  // 0 = no note-off (relies on a one-shot envelope)
    u8 note = 60;
    u8 vel = 100;
};

inline constexpr int MAX_GESTURE_NOTES = 8;

// --------------------------------------------------------------------- variant

struct Variant {
    u16 patch_index = 0;  // into SoundBank::patches
    u16 weight = 1;       // relative odds when the caller passes variant = -1
    std::vector<GestureNote> gesture;

    // Filled by the cache build. 0 while unknown or when the sound is live.
    u32 duration_blocks = 0;
    // Index into PcmCache::entries, or -1 when this variant plays live.
    i32 cache_index = -1;
};

// ----------------------------------------------------------------------- sound

// What happens when a sound is asked to play while it is already at its cap.
enum class OnLimit : u8 {
    Reject = 0,      // the new trigger is refused; the playing ones continue
    StealOldest = 1  // the oldest instance of this sound is cut short
};

struct SoundDef {
    std::string name;
    u8 bus = 0;
    i32 gain_q16 = GAIN_ONE;

    // Higher survives. Only priority beats age when a bus is full, and a
    // higher-priority instance is never stolen for a lower-priority one --
    // that play is rejected instead, which is what makes the number mean
    // something to a designer.
    u8 priority = 128;

    u8 max_instances = 4;
    OnLimit on_limit = OnLimit::Reject;
    u32 cooldown_blocks = 0;

    i32 pitch_jitter_cents_q16 = 0;
    i32 gain_jitter_db_q16 = 0;

    // Effective: synthesize per instance instead of playing PCM. The cache
    // also sets this when a sound could not be cached.
    bool live = false;
    // What the AUTHOR asked for, which is a different question.
    //
    // A sound the author marked live gets the caller's seed, so its noise
    // differs every trigger -- that is what live is for. A sound that merely
    // fell back to live, because the cache budget ran out or because caching
    // was disabled, must sound exactly like the cached version it replaced,
    // so it gets the cache's canonical seed instead. Without that distinction
    // a budget change would audibly alter sounds nobody edited.
    bool live_authored = false;
    bool loop = false;

    Attenuation atten;

    i32 duck_gain_q16 = GAIN_ONE;  // GAIN_ONE = this sound does not duck music
    u32 duck_blocks = 0;

    u16 variant_first = 0;
    u16 variant_count = 0;

    // Tool-only, stripped from .sfxb.
    std::vector<std::string> tags;

    // The body this sound rings with. Empty for most sounds: a resonator bank
    // is what makes something sound STRUCK rather than merely loud, and not
    // everything is struck.
    ResonatorBank resonators;
};

// ------------------------------------------------------------------------- bus

// A bus is a voice budget with a gain on it. Stealing never crosses one, which
// is the whole point: a hundred world sounds cannot silence the UI click or
// the player's own weapon, however the priorities were set.
struct BusDef {
    std::string name;
    i32 gain_q16 = GAIN_ONE;
    u16 max_instances = 16;
    bool spatial = true;  // false = the position argument is ignored, pan is direct
    i32 send_reverb_q16 = 0;
    // Chorus is the "many voices from one" effect -- detuned, delayed copies.
    // On a bus it is what turns a single synthesized voice into a choir, and
    // what gives a machine its beating, unsteady character.
    i32 send_chorus_q16 = 0;
    i32 send_delay_q16 = 0;
};

// ------------------------------------------------------------------------ bank

struct SoundBank {
    std::string name;
    std::vector<BusDef> buses;
    std::vector<SoundDef> sounds;
    std::vector<Variant> variants;

    // Owned patches, referenced by Variant::patch_index. Held by pointer
    // because the cache build destroys the ones it no longer needs -- a
    // wavetable set is ~542 KB per patch and dwarfs the PCM it renders to.
    std::vector<std::unique_ptr<Patch>> patches;

    // The source JSON of each patch, parallel to `patches`. Kept because the
    // baker has to write it back out and the tool has to edit it; dropped by
    // the cache build once nothing needs to re-render.
    std::vector<std::string> patch_json;

    msynth::ReverbParams reverb;
    msynth::ChorusParams chorus;
    msynth::DelayParams delay;
    msynth::MasterParams master;  // `master.enabled` is the bypass; default off

    int sound_index(const std::string& n) const {
        for (size_t i = 0; i < sounds.size(); ++i)
            if (sounds[i].name == n) return static_cast<int>(i);
        return -1;
    }
    int bus_index(const std::string& n) const {
        for (size_t i = 0; i < buses.size(); ++i)
            if (buses[i].name == n) return static_cast<int>(i);
        return -1;
    }
};

}  // namespace dsnd
