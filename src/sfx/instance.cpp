#include "instance.h"

namespace dsnd {
namespace {

// Which layers of a patch answer to this velocity. midisynth's engine does
// the same test at note-on; a layer outside the window is simply not started,
// which is how velocity switching works.
int matching_layers(const msynth::Patch& p, u8 vel, const msynth::LayerDef** out) {
    int n = 0;
    for (u8 i = 0; i < p.layer_count; ++i) {
        const msynth::LayerDef& l = p.layers[i];
        if (vel >= l.vel_low && vel <= l.vel_high) out[n++] = &l;
    }
    return n;
}

int unison_count(const msynth::LayerDef& l) {
    return l.unison_voices < 1 ? 1 : l.unison_voices;
}

}  // namespace

bool advance_gesture(Instance& inst, const SoundBank& bank, SfxVoicePool& pool,
                     const ChannelState& ch, i32 extra_cents_q16) {
    const SoundDef& snd = bank.sounds[static_cast<size_t>(inst.sound)];
    const Variant& var = bank.variants[snd.variant_first + inst.variant];
    const msynth::Patch& patch = *bank.patches[var.patch_index];

    // ---- note-offs that have come due ----
    for (u8 i = 0; i < inst.off_count;) {
        if (inst.offs[i].at_blocks <= inst.age_blocks) {
            for (int v = 0; v < pool.capacity(); ++v) {
                Voice& voice = pool.voice(v);
                if (voice.active && voice.group_id == inst.offs[i].group)
                    voice.note_off(false);
            }
            inst.offs[i] = inst.offs[inst.off_count - 1];
            --inst.off_count;
        } else {
            ++i;
        }
    }

    // ---- gesture notes that have come due ----
    bool ok = true;
    while (inst.next_note < var.gesture.size() &&
           var.gesture[inst.next_note].at_blocks <= inst.age_blocks) {
        const GestureNote& g = var.gesture[inst.next_note];

        const msynth::LayerDef* layers[MAX_LAYERS];
        const int nl = matching_layers(patch, g.vel, layers);
        int want = 0;
        for (int i = 0; i < nl; ++i) want += unison_count(*layers[i]);

        if (want == 0) {
            // No layer answers this velocity. Not an error -- it is what a
            // velocity-switched patch is for -- but there is nothing to start.
            ++inst.next_note;
            continue;
        }

        Voice* got[MAX_SFX_VOICES];
        if (!pool.alloc_n(want, inst.owner, got)) {
            // All or nothing: a half-started note is a worse artefact than a
            // missing one, and alloc_n has already left the pool untouched.
            ok = false;
            break;
        }

        const u32 group = ++inst.group_base;
        int k = 0;
        for (int i = 0; i < nl; ++i) {
            const msynth::LayerDef& l = *layers[i];
            const int un = unison_count(l);
            for (int u = 0; u < un; ++u, ++k) {
                Voice& v = *got[k];
                v.start(v.slot, 0, g.note, g.vel, group, static_cast<u8>(i), &patch, &l, ch);

                // The whole reason this project exists downstream of
                // midisynth: replace the seeds Voice::start derived from the
                // pool slot with ones derived from the caller's seed.
                reseed_voice(v, l, mix_seed(inst.seed, inst.variant, static_cast<u32>(i),
                                            static_cast<u32>(u)));

                // Per-instance pitch offset (jitter, or a caller's request).
                // Carried as a tuning offset rather than a bend, because bend
                // would need a bend range and a range change also moves the
                // wavetable mip chosen at note-on.
                if (extra_cents_q16 != 0) v.base_cents_q16 += extra_cents_q16;

                // Unison spread: midisynth's engine sets these outside
                // Voice::start, so we must too or a unison stack collapses to
                // one detune.
                if (un > 1) {
                    const i32 span = l.unison_detune_cents_q16;
                    const i32 pos = (un == 1) ? 0 : (2 * u * GAIN_ONE / (un - 1) - GAIN_ONE);
                    v.unison_extra_cents_q16 =
                        static_cast<i32>(static_cast<i64>(span) * pos / GAIN_ONE);
                    v.unison_pan_q16 =
                        static_cast<i32>(static_cast<i64>(l.unison_spread_q16) * pos / GAIN_ONE);
                }
            }
        }

        // durMs 0 means no note-off at all: the envelope ends the voice. That
        // is what a one-shot patch is for, and it is the common case.
        if (g.dur_blocks > 0 && inst.off_count < MAX_GESTURE_NOTES) {
            inst.offs[inst.off_count].at_blocks = inst.age_blocks + g.dur_blocks;
            inst.offs[inst.off_count].group = group;
            ++inst.off_count;
        }
        ++inst.next_note;
    }
    return ok;
}

bool render_sources(Instance& inst, const SoundBank& bank, SfxVoicePool& pool,
                    const ChannelState& ch) {
    inst.clear_lane();

    // Built on the first block of a live instance. Doing it here rather than at
    // trigger time keeps every caller of this function -- the engine and the
    // cache builder alike -- on one path.
    if (inst.live && !inst.reson_ready) {
        inst.reson.prepare(bank.sounds[static_cast<size_t>(inst.sound)].resonators);
        inst.reson_ready = true;
    }

    if (!inst.live) {
        const bool alive = inst.sample.render_block(inst.lane_l, inst.lane_r);
        return alive;
    }

    // The live path accumulates every owned voice into the lane. The send
    // buses are pointed at a scratch block that is thrown away: a per-layer
    // send override would otherwise write into memory we never read, and
    // Voice::render_block dereferences those pointers whenever a send is
    // nonzero. The instance's send is applied downstream, by its bus, once.
    i64 scratch[CONTROL_BLOCK] = {};
    msynth::VoiceBuses buses{inst.lane_l, inst.lane_r, scratch, scratch, scratch,
                             scratch,     scratch,    scratch, 120 << GAIN_SHIFT, nullptr};

    const u16 owner = inst.owner;
    bool any = false;
    for (int i = 0; i < pool.capacity(); ++i) {
        if (pool.owner(i) != owner) continue;
        Voice& v = pool.voice(i);
        if (!v.active) continue;
        v.render_block(ch, buses);
        if (v.active) any = true;
    }

    // The body, rung by the exciter the voices just produced.
    //
    // HERE and nowhere else, because this is the one place a sound is
    // SYNTHESIZED -- the cache builds itself by calling this same function
    // with inst.live set, so the modes are baked into the cached samples and
    // cached playback (which returns above) must not ring them again. That is
    // what keeps "a cached sound is bit-identical to its live render" true
    // with a resonator in the path.
    const SoundDef& sd = bank.sounds[static_cast<size_t>(inst.sound)];
    if (!sd.resonators.empty()) {
        inst.reson.process(inst.lane_l, inst.lane_r, CONTROL_BLOCK);
        // A ringing body outlives the voices that struck it, so the instance
        // is not finished while any mode still has energy in it. Without this
        // the tail would be cut at the exciter's end -- which is exactly the
        // part a resonator exists to provide.
        if (!any && inst.reson.ringing()) any = true;
    }

    // Still to come counts as alive, or a gesture with a gap in it would end
    // at the gap.
    const SoundDef& snd = bank.sounds[static_cast<size_t>(inst.sound)];
    const Variant& var = bank.variants[snd.variant_first + inst.variant];
    if (inst.next_note < var.gesture.size()) any = true;
    return any;
}

}  // namespace dsnd
