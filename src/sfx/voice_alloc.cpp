#include "voice_alloc.h"

namespace dsnd {

ChannelState neutral_channel() {
    ChannelState ch;
    ch.program = 0;
    ch.is_drum = false;
    ch.volume = 127;
    ch.expression = 127;
    ch.pan_q16 = 0;
    ch.send_reverb = 0;
    ch.send_chorus = 0;
    ch.send_delay = 0;
    ch.bend_raw = 0;
    ch.bend_range_cents = 200;
    ch.fine_tune_cents_q16 = 0;
    ch.coarse_tune_cents = 0;
    ch.modwheel_q16 = 0;
    ch.aftertouch_q16 = 0;
    ch.cutoff_offset_q16 = 0;
    ch.res_offset_q16 = 0;
    ch.sustain = false;
    ch.portamento = false;
    ch.fade_gain_q16 = GAIN_ONE;
    ch.fade_target_q16 = GAIN_ONE;
    ch.fade_step_q16 = 0;
    return ch;
}

u32 mix_seed(u32 seed, u32 variant, u32 layer, u32 unison) {
    // A plain avalanche over the four inputs. The requirement is only that
    // two different (variant, layer, unison) triples of one trigger do not
    // share a stream -- layers sharing a noise stream sounds like phasing --
    // and that the result depends on nothing else.
    u32 h = seed ? seed : 0x9E3779B9u;
    h ^= 0x9E3779B9u * (variant + 1);
    h ^= 0x85EBCA6Bu * (layer + 1);
    h ^= 0xC2B2AE35u * (unison + 1);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h ? h : 0x9E3779B9u;
}

void reseed_voice(Voice& v, const LayerDef& l, u32 seed, u64 blocks_elapsed, gain_t bpm_q16) {
    v.noise = msynth::XorShift32(seed);
    // The coloured-noise filter state is part of the noise generator, and
    // start() has already zeroed it; re-zero anyway so that reseeding is a
    // complete statement about the generator rather than half of one.
    v.noise_b0 = v.noise_b1 = v.noise_b2 = 0;

    // Same derivation midisynth uses per LFO, so the relationship between a
    // voice's LFO streams is unchanged; only the root moves.
    for (u8 i = 0; i < l.lfo_count && i < msynth::MAX_LFOS; ++i)
        v.lfo[i].note_on(l.lfos[i], seed ^ (0x9E3779B9u * (i + 1)), blocks_elapsed, bpm_q16);
}

SfxVoicePool::SfxVoicePool() : voices_(new Voice[MAX_SFX_VOICES]) {
    for (int i = 0; i < MAX_SFX_VOICES; ++i) {
        voices_[i].slot = static_cast<u8>(i);
        owner_[i] = NO_OWNER;
        armed_[i] = false;
    }
}

bool SfxVoicePool::alloc_n(int n, u16 owner, Voice** out) {
    if (n <= 0 || n > MAX_SFX_VOICES) return false;

    // Two passes rather than one: find them all before claiming any, so a
    // failed allocation leaves the pool exactly as it was. A partially
    // claimed instance would have to be unwound by the caller, and a caller
    // that forgot would leak voices no owner ever releases.
    int found = 0;
    for (int i = 0; i < MAX_SFX_VOICES && found < n; ++i)
        if (owner_[i] == NO_OWNER && !voices_[i].active) out[found++] = &voices_[i];
    if (found < n) return false;

    for (int i = 0; i < n; ++i) {
        owner_[out[i]->slot] = owner;
        armed_[out[i]->slot] = false;
    }
    const int live = active_count();
    if (live > peak_) peak_ = live;
    return true;
}

void SfxVoicePool::release_instance(u16 owner) {
    if (owner == NO_OWNER) return;
    for (int i = 0; i < MAX_SFX_VOICES; ++i) {
        if (owner_[i] != owner) continue;
        if (voices_[i].active) {
            // Ownership is kept until the voice actually goes idle, so the
            // fade is heard and the slot is not handed out mid-ramp.
            voices_[i].fast_release();
            armed_[i] = true;
        } else if (!armed_[i]) {
            // Reserved but never started -- the caller changed its mind, or
            // its layer had nothing to play. Hand it straight back; there is
            // no tail to wait for.
            owner_[i] = NO_OWNER;
        } else {
            owner_[i] = NO_OWNER;
            armed_[i] = false;
        }
    }
}

void SfxVoicePool::release_all() {
    for (int i = 0; i < MAX_SFX_VOICES; ++i) {
        if (voices_[i].active) {
            voices_[i].fast_release();
            armed_[i] = true;
        }
    }
}

void SfxVoicePool::reset() {
    // EVERY field, not just `active`. A voice's envelopes start their attack
    // from their current level -- deliberately, so a retriggered note does
    // not click -- and an offline render ends on SILENCE, which can leave a
    // voice inaudible but still mid-envelope. Clearing only `active` kept that
    // level, so the next sound allocated into the slot started its attack
    // part-way up: the same sound rendered differently depending on what the
    // editor (or the fitter) had rendered before it. A fresh pool -- what
    // PcmCache::build uses -- never had the problem, so the fix is to make a
    // reset pool indistinguishable from a fresh one.
    for (int i = 0; i < MAX_SFX_VOICES; ++i) {
        voices_[i] = Voice{};
        voices_[i].slot = static_cast<u8>(i);
        owner_[i] = NO_OWNER;
        armed_[i] = false;
    }
}

void SfxVoicePool::reap() {
    for (int i = 0; i < MAX_SFX_VOICES; ++i) {
        if (owner_[i] == NO_OWNER) continue;
        if (voices_[i].active) {
            armed_[i] = true;
        } else if (armed_[i]) {
            owner_[i] = NO_OWNER;
            armed_[i] = false;
        }
        // Owned, idle and never armed: reserved this block, not yet started.
        // Leaving it alone is the whole point of arming.
    }
}

int SfxVoicePool::free_count() const {
    int n = 0;
    for (int i = 0; i < MAX_SFX_VOICES; ++i)
        if (owner_[i] == NO_OWNER && !voices_[i].active) ++n;
    return n;
}

int SfxVoicePool::active_count() const {
    int n = 0;
    for (int i = 0; i < MAX_SFX_VOICES; ++i)
        if (owner_[i] != NO_OWNER) ++n;
    return n;
}

}  // namespace dsnd
