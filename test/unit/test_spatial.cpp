//
// Placing a sound: distance, direction, and the air between.
//
// These are table-driven rather than golden, because the properties are the
// point -- unity at the near distance, silence at the far one, monotonic in
// between, constant power across the pan. A hash would pin the numbers
// without saying which of them were meant.
//
#include "sfx/spatial.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

constexpr i32 U(int units) { return units * GAIN_ONE; }

Attenuation lin(int near_u, int far_u) {
    Attenuation a;
    a.min_dist_q16 = U(near_u);
    a.max_dist_q16 = U(far_u);
    a.curve = CurveId::Linear;
    return a;
}

}  // namespace

TEST(distance_is_exact_for_the_obvious_cases) {
    CHECK_EQ(distance_q16(0, 0, 0), 0);
    CHECK_EQ(distance_q16(U(3), 0, 0), U(3));
    CHECK_EQ(distance_q16(0, U(5), 0), U(5));
    CHECK_EQ(distance_q16(0, 0, U(7)), U(7));
    // 3-4-5.
    CHECK_EQ(distance_q16(U(3), U(4), 0), U(5));
    // Sign does not matter.
    CHECK_EQ(distance_q16(-U(3), -U(4), 0), U(5));
}

TEST(distance_gain_is_unity_near_and_silent_far) {
    const Attenuation a = lin(4, 40);
    CHECK_EQ(distance_gain(a, 0), GAIN_ONE);
    CHECK_EQ(distance_gain(a, U(4)), GAIN_ONE);
    CHECK_EQ(distance_gain(a, U(2)), GAIN_ONE);  // inside the near radius
    CHECK_EQ(distance_gain(a, U(40)), 0);
    CHECK_EQ(distance_gain(a, U(4000)), 0);
}

TEST(distance_gain_is_monotonic) {
    for (CurveId c : {CurveId::Linear, CurveId::Exp, CurveId::ExpAttack, CurveId::SCurve}) {
        Attenuation a = lin(4, 40);
        a.curve = c;
        i32 prev = GAIN_ONE + 1;
        for (int d = 0; d <= 44; ++d) {
            const i32 g = distance_gain(a, U(d));
            CHECK(g <= prev);
            CHECK(g >= 0);
            CHECK(g <= GAIN_ONE);
            prev = g;
        }
    }
}

TEST(distance_gain_halfway_is_halfway_on_a_linear_curve) {
    const Attenuation a = lin(0, 40);
    CHECK_NEAR(distance_gain(a, U(20)), GAIN_ONE / 2, GAIN_ONE / 200);
}

// The defaults are Doom's own S_CLOSE_DIST and S_CLIPPING_DIST at the
// engine's 1/48 scale, so a converted map sounds the way it was placed.
TEST(default_attenuation_matches_dooms_distances) {
    const Attenuation a;
    CHECK_NEAR(a.min_dist_q16, (160 * GAIN_ONE) / 48, GAIN_ONE / 100);
    CHECK_NEAR(a.max_dist_q16, (1200 * GAIN_ONE) / 48, GAIN_ONE / 100);
}

// ------------------------------------------------------------------- panning

TEST(a_source_dead_ahead_is_centred) {
    Listener l;  // at the origin, facing +X
    const Attenuation a = lin(1, 100);
    const Placement p = place(l, a, U(10), 0, 0);
    CHECK_EQ(p.pan_q16, 0);
    CHECK(p.gain_q16 > 0);
}

TEST(pan_follows_the_listeners_facing) {
    Listener l;
    const Attenuation a = lin(1, 100);

    // Facing +X, a source at +Y is to the listener's LEFT in a right-handed
    // Z-up frame, so the pan is negative.
    const Placement left = place(l, a, 0, U(10), 0);
    CHECK(left.pan_q16 < 0);
    const Placement right = place(l, a, 0, -U(10), 0);
    CHECK(right.pan_q16 > 0);
    // Symmetric.
    CHECK_EQ(left.pan_q16, -right.pan_q16);

    // Turn the listener a quarter turn and the same source moves to centre.
    l.yaw_brad = 0x40000000u;  // +90 degrees, now facing +Y
    const Placement ahead = place(l, a, 0, U(10), 0);
    CHECK_NEAR(ahead.pan_q16, 0, GAIN_ONE / 500);
}

TEST(pan_is_hard_over_when_the_source_is_abeam) {
    Listener l;
    const Attenuation a = lin(1, 100);
    const Placement p = place(l, a, 0, -U(50), 0);
    CHECK_NEAR(p.pan_q16, GAIN_ONE, GAIN_ONE / 200);
}

TEST(spread_narrows_the_image) {
    Listener l;
    Attenuation a = lin(1, 100);
    const Placement wide = place(l, a, 0, -U(50), 0);
    a.spread_q16 = GAIN_ONE / 2;
    const Placement narrow = place(l, a, 0, -U(50), 0);
    CHECK(narrow.pan_q16 < wide.pan_q16);
    CHECK_NEAR(narrow.pan_q16, wide.pan_q16 / 2, GAIN_ONE / 200);
    a.spread_q16 = 0;
    CHECK_EQ(place(l, a, 0, -U(50), 0).pan_q16, 0);
}

// Walking through a source must not swing it hard from one ear to the other.
// This is the most noticeable artefact a naive panner has, and the collapse
// toward centre inside minDist is what prevents it.
TEST(pan_collapses_to_centre_as_the_listener_enters_a_source) {
    Listener l;
    const Attenuation a = lin(8, 80);
    i32 prev = GAIN_ONE + 1;
    for (int d = 8; d >= 0; --d) {
        const Placement p = place(l, a, 0, -U(d), 0);
        const i32 mag = p.pan_q16 < 0 ? -p.pan_q16 : p.pan_q16;
        CHECK(mag <= prev);
        prev = mag;
    }
    CHECK_EQ(place(l, a, 0, 0, 0).pan_q16, 0);
}

// Control: without the collapse the pan would stay hard over right up to the
// listener's position. This shows the effect is real, not a rounding artefact.
TEST(control_pan_stays_wide_outside_the_near_radius) {
    Listener l;
    const Attenuation a = lin(8, 80);
    const Placement far = place(l, a, 0, -U(40), 0);
    const Placement edge = place(l, a, 0, -U(8), 0);
    CHECK_NEAR(far.pan_q16, GAIN_ONE, GAIN_ONE / 200);
    CHECK_NEAR(edge.pan_q16, GAIN_ONE, GAIN_ONE / 200);
}

TEST(rear_attenuation_separates_front_from_back) {
    Listener l;
    Attenuation a = lin(1, 100);
    a.rear_gain_q16 = GAIN_ONE / 2;  // -6 dB behind

    const Placement ahead = place(l, a, U(10), 0, 0);
    const Placement behind = place(l, a, -U(10), 0, 0);
    // Same distance, so only the rear term differs.
    CHECK(behind.gain_q16 < ahead.gain_q16);
    CHECK_NEAR(behind.gain_q16, ahead.gain_q16 / 2, GAIN_ONE / 100);

    // Abeam is neither, so it is untouched.
    const Placement abeam = place(l, a, 0, U(10), 0);
    CHECK_NEAR(abeam.gain_q16, ahead.gain_q16, GAIN_ONE / 200);
}

TEST(rear_attenuation_off_by_default) {
    Listener l;
    const Attenuation a = lin(1, 100);
    CHECK_EQ(place(l, a, U(10), 0, 0).gain_q16, place(l, a, -U(10), 0, 0).gain_q16);
}

TEST(height_folds_into_distance_only) {
    Listener l;
    const Attenuation a = lin(1, 100);
    const Placement above = place(l, a, 0, 0, U(10));
    CHECK_EQ(above.pan_q16, 0);  // no height cue in stereo, and none pretended
    CHECK_EQ(above.gain_q16, place(l, a, U(10), 0, 0).gain_q16);
}

// ---------------------------------------------------------------- air filter

TEST(air_filter_is_an_exact_bypass_when_unset) {
    AirFilter f;
    i64 l[CONTROL_BLOCK], r[CONTROL_BLOCK], ref[CONTROL_BLOCK];
    for (int i = 0; i < CONTROL_BLOCK; ++i) l[i] = r[i] = ref[i] = (i % 7) * 100000 - 300000;
    f.process(l, r);
    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        CHECK_EQ(l[i], ref[i]);
        CHECK_EQ(r[i], ref[i]);
    }
}

TEST(air_filter_removes_high_frequencies) {
    AirFilter f;
    f.set_hz(500 * GAIN_ONE);
    CHECK(f.a_q30 > 0);

    // Alternating full-scale: the highest frequency the rate can carry.
    i64 l[CONTROL_BLOCK], r[CONTROL_BLOCK];
    i64 in_peak = 0, out_peak = 0;
    for (int pass = 0; pass < 8; ++pass) {
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            l[i] = r[i] = ((i & 1) ? 1 : -1) * (1 << SAMPLE_SHIFT);
            const i64 a = l[i] < 0 ? -l[i] : l[i];
            if (a > in_peak) in_peak = a;
        }
        f.process(l, r);
    }
    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        const i64 a = l[i] < 0 ? -l[i] : l[i];
        if (a > out_peak) out_peak = a;
    }
    CHECK(out_peak * 4 < in_peak);  // heavily attenuated
}

TEST(air_filter_passes_dc_through) {
    AirFilter f;
    f.set_hz(2000 * GAIN_ONE);
    i64 l[CONTROL_BLOCK], r[CONTROL_BLOCK];
    const i64 dc = 1 << (SAMPLE_SHIFT - 2);
    for (int pass = 0; pass < 200; ++pass) {
        for (int i = 0; i < CONTROL_BLOCK; ++i) l[i] = r[i] = dc;
        f.process(l, r);
    }
    // A low-pass settles at its input, give or take the fixed-point floor.
    CHECK_NEAR(l[CONTROL_BLOCK - 1], dc, dc / 100);
}

TEST(air_filter_corner_opens_with_proximity) {
    Listener l;
    Attenuation a = lin(4, 40);
    a.air_lpf_hz_q16 = 800 * GAIN_ONE;

    const Placement near = place(l, a, U(4), 0, 0);
    const Placement mid = place(l, a, U(22), 0, 0);
    const Placement far = place(l, a, U(39), 0, 0);
    CHECK(near.lpf_hz_q16 > mid.lpf_hz_q16);
    CHECK(mid.lpf_hz_q16 > far.lpf_hz_q16);

    // At the near radius the filter is wide open -- an unfiltered source.
    CHECK(near.lpf_hz_q16 > 19000 * GAIN_ONE);
    // The corner slides linearly toward the authored value and only arrives
    // at maxDist, where the source is silent anyway. At 39 of 40 units it is
    // most of the way there, not all of it.
    CHECK(far.lpf_hz_q16 < 2000 * GAIN_ONE);
    CHECK(far.lpf_hz_q16 > a.air_lpf_hz_q16);
}
