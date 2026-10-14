#include "spatial.h"

namespace dsnd {
namespace {

// Beyond this the geometry stops mattering and the squares stop fitting
// comfortably in 64 bits. No map is this big; a source further away is
// silent long before it is far.
constexpr i32 MAX_COORD_Q16 = 4096 * GAIN_ONE;

i32 clamp_coord(i32 v) { return v < -MAX_COORD_Q16 ? -MAX_COORD_Q16
                                                   : (v > MAX_COORD_Q16 ? MAX_COORD_Q16 : v); }

}  // namespace

i32 distance_q16(i32 dx, i32 dy, i32 dz) {
    dx = clamp_coord(dx);
    dy = clamp_coord(dy);
    dz = clamp_coord(dz);
    const dnp::u128 d2 = static_cast<dnp::u128>(static_cast<i64>(dx) * dx +
                                                static_cast<i64>(dy) * dy +
                                                static_cast<i64>(dz) * dz);
    // sqrt of a Q32.32 square is a Q16.16 length, exactly.
    return static_cast<i32>(dnp::isqrt128(d2));
}

i32 distance_gain(const Attenuation& a, i32 dist_q16) {
    if (dist_q16 <= a.min_dist_q16) return GAIN_ONE;
    if (dist_q16 >= a.max_dist_q16) return 0;

    const i64 span = static_cast<i64>(a.max_dist_q16) - a.min_dist_q16;
    if (span <= 0) return GAIN_ONE;
    // Normalised 0..1 across the falloff, Q16.16.
    i64 t = (static_cast<i64>(dist_q16 - a.min_dist_q16) * GAIN_ONE) / span;
    if (t < 0) t = 0;
    if (t > GAIN_ONE) t = GAIN_ONE;

    // curve_eval wants a Q24 phase and its tables run 0 -> GAIN_ONE, so the
    // curve is inverted: full gain near, silence far.
    const gain_t* curve = msynth::curve_table(a.curve);
    const gain_t c = msynth::curve_eval(curve, static_cast<u32>(t) << 8);
    return GAIN_ONE - c;
}

Placement place(const Listener& l, const Attenuation& a, i32 sx, i32 sy, i32 sz) {
    Placement p;

    const i32 dx = sx - l.x_q16;
    const i32 dy = sy - l.y_q16;
    const i32 dz = sz - l.z_q16;
    const i32 dist = distance_q16(dx, dy, dz);

    p.gain_q16 = distance_gain(a, dist);
    if (p.gain_q16 == 0) {
        p.pan_q16 = 0;
        return p;
    }

    // The listener's basis, from the same full-turn sine table the LFOs use.
    // A brad IS a u32 phase, so this is a table lookup, not trigonometry.
    const i32 fwd_x = msynth::asr(msynth::sine_q30(l.yaw_brad + 0x40000000u), COEF_SHIFT - GAIN_SHIFT);
    const i32 fwd_y = msynth::asr(msynth::sine_q30(l.yaw_brad), COEF_SHIFT - GAIN_SHIFT);
    // Right-hand: right = (forward.y, -forward.x).
    const i32 right_x = fwd_y;
    const i32 right_y = -fwd_x;

    if (dist > 0) {
        // The lateral direction cosine, in [-1, 1]. Fed straight into the
        // constant-power pan table, this IS the sine-pan law -- which is why
        // there is no atan2 here and no need for core's fx_atan2 (whose .cpp
        // would drag a second project into this build for nothing).
        const i64 lat = (static_cast<i64>(dx) * right_x + static_cast<i64>(dy) * right_y) /
                        GAIN_ONE;
        i64 pan = (lat * GAIN_ONE) / dist;
        if (pan > GAIN_ONE) pan = GAIN_ONE;
        if (pan < -GAIN_ONE) pan = -GAIN_ONE;

        pan = (pan * a.spread_q16) / GAIN_ONE;

        // Collapse toward centre as the listener enters the source. Without
        // this, walking through a sound swings it hard from one ear to the
        // other over a few centimetres, which is the single most noticeable
        // artefact a naive panner has.
        if (a.min_dist_q16 > 0 && dist < a.min_dist_q16)
            pan = (pan * dist) / a.min_dist_q16;

        p.pan_q16 = static_cast<i32>(pan);

        // Front/back is genuinely ambiguous in stereo -- directly behind pans
        // exactly like directly in front. `rearDb` is not a cue so much as a
        // disambiguation: enough to tell the two apart, cheaply.
        if (a.rear_gain_q16 < GAIN_ONE) {
            const i64 front = (static_cast<i64>(dx) * fwd_x + static_cast<i64>(dy) * fwd_y) /
                              GAIN_ONE;
            const i64 f = (front * GAIN_ONE) / dist;  // -1 behind .. +1 ahead
            if (f < 0) {
                const i64 amount = -f > GAIN_ONE ? GAIN_ONE : -f;
                const i64 g = GAIN_ONE + ((static_cast<i64>(a.rear_gain_q16) - GAIN_ONE) * amount) /
                                             GAIN_ONE;
                p.gain_q16 = static_cast<i32>((static_cast<i64>(p.gain_q16) * g) / GAIN_ONE);
            }
        }
    }

    // Air absorption scales with how far through the falloff we are, so a
    // source at minDist is unfiltered and one at maxDist is fully filtered.
    if (a.air_lpf_hz_q16 > 0) {
        const i64 span = static_cast<i64>(a.max_dist_q16) - a.min_dist_q16;
        i64 t = span > 0 ? (static_cast<i64>(dist - a.min_dist_q16) * GAIN_ONE) / span : 0;
        if (t < 0) t = 0;
        if (t > GAIN_ONE) t = GAIN_ONE;
        // Interpolate the CORNER from wide open down to the authored value.
        const i64 open = 20000LL * GAIN_ONE;
        p.lpf_hz_q16 = static_cast<i32>(open + ((static_cast<i64>(a.air_lpf_hz_q16) - open) * t) /
                                                   GAIN_ONE);
    }
    return p;
}

}  // namespace dsnd
