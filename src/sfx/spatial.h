#pragma once
//
// Placing a sound in the world: distance, direction, and the air between.
//
// All of it in fixed point, out of midisynth's baked tables, with no libm and
// no atan2 -- so a sound placed at a given point produces the same bytes on
// every target, which is what the golden renders are for.
//
// COORDINATE SPACE is the SIMULATION's: X and Y on the ground, Z up, Q16.16
// world units, and the listener's facing as a `brad` yaw about +Z. It is NOT
// the renderer's space. Positions originate in the simulation on both client
// and server, the offline renderer has no renderer at all, and the one
// sim->Three mapping already exists in client/src/mapgeom.js; a second,
// inverse copy inside a C++ library would be a convention with two homes and
// no owner. An embedder must take the SIM yaw -- the camera's is a different
// convention and will pan everything backwards.
//
// Scale, for anyone authoring distances: 1 world unit is 48 Doom units,
// roughly 1.46 m.
//
#include "config.h"
#include "dnp/math/fixed.hpp"
#include "sound_def.h"
#include "synth/tables.h"

namespace dsnd {

struct Listener {
    i32 x_q16 = 0, y_q16 = 0, z_q16 = 0;
    u32 yaw_brad = 0;  // 0 = facing +X, per core/include/dnp/math/trig.hpp
};

// What the spatialiser decided for one instance this block.
struct Placement {
    i32 gain_q16 = GAIN_ONE;  // distance x rear, before the instance's own gain
    i32 pan_q16 = 0;          // -GAIN_ONE (hard left) .. +GAIN_ONE (hard right)
    i32 lpf_hz_q16 = 0;       // 0 = no air filter
};

// Distance between two points, Q16.16, via the engine's own integer sqrt.
i32 distance_q16(i32 dx, i32 dy, i32 dz);

// Gain from distance: GAIN_ONE at or inside minDist, 0 at or beyond maxDist,
// shaped by the attenuation curve in between. The curve tables are the same
// ones envelopes use, so `exp` here and `exp` there mean the same shape.
i32 distance_gain(const Attenuation& a, i32 dist_q16);

// The whole placement for one source.
Placement place(const Listener& l, const Attenuation& a, i32 sx, i32 sy, i32 sz);

// A one-pole low-pass in Q8.24 -- the lane's own format.
//
// Not the SVF: FilterState is Q2.30 in and out, so an instance lane (Q8.24,
// and able to exceed 2.0 when layers sum) would need a shift and a clamp on
// every sample, and a resonant 12 dB/oct is the wrong shape for air
// absorption anyway. The corner still comes from the baked cutoff table, so
// nothing here computes a tangent.
struct AirFilter {
    i32 y_l = 0, y_r = 0;
    i32 a_q30 = 0;  // 0 = pass through untouched

    void set_hz(i32 hz_q16) {
        if (hz_q16 <= 0) {
            a_q30 = 0;
            return;
        }
        const msynth::coef_t g = msynth::cutoff_g(msynth::cutoff_index_from_hz(hz_q16));
        a_q30 = static_cast<i32>((static_cast<i64>(g) << COEF_SHIFT) /
                                 (static_cast<i64>(msynth::COEF_ONE) + g));
    }

    void reset() { y_l = y_r = 0; }

    void process(i64* l, i64* r) {
        if (a_q30 == 0) return;  // exact bypass: an unset filter changes nothing
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            y_l += static_cast<i32>(msynth::asr((l[i] - y_l) * a_q30, COEF_SHIFT));
            y_r += static_cast<i32>(msynth::asr((r[i] - y_r) * a_q30, COEF_SHIFT));
            l[i] = y_l;
            r[i] = y_r;
        }
    }
};

}  // namespace dsnd
