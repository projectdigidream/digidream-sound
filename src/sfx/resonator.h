#ifndef DSND_RESONATOR_H
#define DSND_RESONATOR_H
//
// A modal resonator bank: the body a sound rings with.
//
// WHY THIS EXISTS. The engine could already do subtractive (noise through
// filters), FM, waveshaping and additive synthesis. The one standard technique
// in the procedural-audio literature it had no form of is MODAL SYNTHESIS --
// an exciter feeding a bank of damped resonators, whose impulse response is a
// sum of exponentially decaying sinusoids. That is the canonical way to make
// anything struck or rattled: metal, wood, glass, a casing, a mechanism. Most
// of a game's sound set.
//
// It is not a new oscillator. midisynth's patch is the EXCITER -- it is already
// very good at short broadband bursts -- and this is the resonant body it
// excites. That division is the classical one, and it has a practical virtue
// here: the exciter stays midisynth's, unforked, and the body is ours.
//
// EACH MODE is a two-pole resonator:
//
//     y[n] = a1*y[n-1] - a2*y[n-2] + x[n]
//     a1 = 2*r*cos(w),  a2 = r*r,  w = 2*pi*f/fs
//
// where r is the per-sample decay factor. Chosen so the mode falls 60 dB in
// `decayMs`, which is how a decay time is meant everywhere else in this
// project.
//
// FIXED POINT, like everything in the audio path. Coefficients are Q2.30, the
// same format FilterState uses, and the state is i64 because the lane is. No
// libm: `r` comes from the baked exp2 table and `cos` from the baked sine
// table, so a bank built on one machine is the bank built on every other.
//
#include <dnp/math/fixed.hpp>

#include "config.h"
#include "synth/fixmath.h"
#include "synth/tables.h"

namespace dsnd {

// Eight is not arbitrary: a struck object's first handful of modes carry its
// identity, and past that they are below the noise of everything else in a
// mix. It also keeps ResonatorState inside one cache line per channel.
inline constexpr int MAX_RESONATORS = 8;

// Authored form, as it appears in a .sfxproj.
struct ResonatorDef {
    i32 hz_q16 = 0;        // 20 .. 12000; above fs/4 the two-pole form misbehaves
    i32 decay_ms_q16 = 0;  // 1 .. 4000, to -60 dB
    i32 gain_q16 = 0;      // 0 .. 4, per mode
};

struct ResonatorBank {
    u8 count = 0;
    ResonatorDef modes[MAX_RESONATORS];
    // How much of the dry exciter survives. 0 is the resonators alone -- a
    // pure body with no click -- and 1 keeps the exciter at full level under
    // them, which is usually what a real impact sounds like.
    i32 dry_q16 = GAIN_ONE;

    bool empty() const { return count == 0; }
};

// Baked coefficients. Computed once at load, never per block.
struct ResonatorCoefs {
    i32 a1 = 0;    // Q2.30, 2*r*cos(w)
    i32 a2 = 0;    // Q2.30, r*r
    i32 gain = 0;  // Q16.16
    // Q2.30, sin(w). Scales the input so `gain` is the amplitude the mode
    // rings at when struck by a unit impulse. See bake_resonator.
    i64 norm = 0;
};

// Extra fractional bits carried in the resonator state.
//
// Normalisation scales a low mode's input down by sin(w) -- 0.0026 at 20 Hz --
// and without extra precision the excitation it rings from would lose most of
// its resolution. Sixteen bits keeps it clean; the products are i128, so the
// headroom costs nothing in range.
inline constexpr int RESO_EXTRA = 16;

/** 2*r*cos(w) and r*r for one mode, from the baked tables only. */
inline ResonatorCoefs bake_resonator(const ResonatorDef& d) noexcept {
    ResonatorCoefs c;
    if (d.hz_q16 <= 0 || d.decay_ms_q16 <= 0 || d.gain_q16 == 0) return c;

    // The two-pole form is only well behaved below fs/4, the same ceiling
    // FilterState documents for its own cutoff.
    i64 hz = d.hz_q16;
    const i64 max_hz = static_cast<i64>(SAMPLE_RATE / 4) << GAIN_SHIFT;
    if (hz > max_hz) hz = max_hz;
    if (hz < (20 << GAIN_SHIFT)) hz = 20 << GAIN_SHIFT;

    // r = exp(-eps), eps = ln(1000) / N, for -60 dB over N samples.
    //
    // NOT through exp2_q16. That returns r in Q16.16, and for a long mode the
    // quantity that matters is 1 - r: 3.6e-5 for a four-second decay, barely
    // two LSBs, so the decay rate came out quantised to within +/-30% -- a
    // mode told to fall 60 dB in 4000 ms fell 76. A pole this close to one is
    // exactly where a Taylor series is accurate: eps is at most 0.144 (a 1 ms
    // decay), where four terms are good to 1e-6, and for any long mode to
    // 1e-12. Q2.30 throughout, integer only, so it is still the same number on
    // every platform.
    const i64 n_samples =
        (static_cast<i64>(d.decay_ms_q16) * SAMPLE_RATE) / 1000;  // Q16.16 samples
    if (n_samples <= 0) return c;
    constexpr i64 LN1000_Q30 = 7417145753LL;                      // ln(1000) in Q2.30
    const i64 one30 = static_cast<i64>(1) << 30;
    const i64 eps = (LN1000_Q30 << GAIN_SHIFT) / n_samples;       // Q2.30
    const i64 e2 = msynth::asr(eps * eps, 30);
    const i64 e3 = msynth::asr(e2 * eps, 30);
    const i64 e4 = msynth::asr(e3 * eps, 30);
    const i64 r30 = one30 - eps + e2 / 2 - e3 / 6 + e4 / 24;      // Q2.30, (0, 1)

    // cos(w) from the sine table a quarter turn early. Phase is the full u32
    // turn, which is how sine_q30 is indexed everywhere.
    const u32 phase = static_cast<u32>(
        (static_cast<u64>(static_cast<u32>(hz)) << 32) / (static_cast<u64>(SAMPLE_RATE) << GAIN_SHIFT));
    const i32 cos_q30 = msynth::sine_q30(phase + 0x40000000u);

    // a1 = 2*r*cos(w), a2 = r*r, all Q2.30.
    c.a1 = static_cast<i32>(msynth::asr(2 * r30 * static_cast<i64>(cos_q30), 30));
    c.a2 = static_cast<i32>(msynth::asr(r30 * r30, 30));
    c.gain = d.gain_q16;

    // NORMALISATION: `gain` is the mode's amplitude when STRUCK.
    //
    // With input scaled by sin(w), the impulse response is exactly
    //
    //     h[n] = r^n * sin((n + 1) w)
    //
    // -- a sinusoid of unit amplitude decaying 60 dB over decayMs. That is the
    // standard modal-synthesis parameterisation, and the right one for a body
    // that is hit: a four-millisecond click into a two-second mode rings for
    // two seconds at the level `gain` says.
    //
    // Two wrong answers came first, and both are worth recording. Unscaled,
    // gain at resonance is 1 / ((1 - r) |1 - r e^(-2jw)|) -- tens of thousands
    // for a long low mode -- and every DOOM role the fitter won with `modal`
    // came out saturated at exactly +42.1 dBFS, the point where Q8.24
    // overflows an i32. Normalising to the steady-state response instead
    // bounded that, but made a long mode almost silent when struck, since a
    // strike carries energy only in proportion to a mode's bandwidth. That is
    // a filter's normalisation, not a struck object's.
    //
    // hz is capped at fs/4, so w is in (0, pi/2] and sin(w) is positive.
    c.norm = msynth::sine_q30(phase);
    if (c.norm <= 0) c.norm = 1;   // never silence a mode by rounding
    return c;
}

/** Per-instance state: two samples of history per mode, per channel. */
struct ResonatorState {
    i64 z1l[MAX_RESONATORS] = {};
    i64 z2l[MAX_RESONATORS] = {};
    i64 z1r[MAX_RESONATORS] = {};
    i64 z2r[MAX_RESONATORS] = {};
    ResonatorCoefs coefs[MAX_RESONATORS] = {};
    // Samples of silence after which a mode is forced to exact zero: twice its
    // declared decay. See `quiet` below.
    u32 limit[MAX_RESONATORS] = {};
    // Below this ring energy a mode is inaudible and is set to zero. See
    // ring_energy().
    dnp::i128 quiet_energy[MAX_RESONATORS] = {};
    // Consecutive samples with no input at all.
    u32 quiet = 0;
    u8 count = 0;
    i32 dry_q16 = GAIN_ONE;

    void prepare(const ResonatorBank& b) noexcept {
        count = b.count > MAX_RESONATORS ? MAX_RESONATORS : b.count;
        dry_q16 = b.dry_q16;
        for (int i = 0; i < count; ++i) {
            coefs[i] = bake_resonator(b.modes[i]);
            // decay_ms (Q16.16) -> samples, doubled. A mode is 60 dB down at
            // its decay and 120 dB down at twice it, which for any input the
            // lane can carry is below one LSB of the 16-bit output.
            const u64 samples = (static_cast<u64>(static_cast<u32>(b.modes[i].decay_ms_q16)) *
                                 SAMPLE_RATE / 1000u) >> GAIN_SHIFT;
            limit[i] = static_cast<u32>(2 * samples + CONTROL_BLOCK);
            // A ring of amplitude A has energy A^2 sin^2(w), so the threshold
            // for amplitude DEADBAND is (DEADBAND sin w)^2.
            const dnp::i128 t = dnp::asr128(static_cast<dnp::i128>(DEADBAND) * coefs[i].norm, 30);
            quiet_energy[i] = t * t;
        }
        clear();
    }

    void clear() noexcept {
        for (int i = 0; i < MAX_RESONATORS; ++i) {
            z1l[i] = z2l[i] = z1r[i] = z2r[i] = 0;
        }
        quiet = 0;
    }

    /**
     * Ring the bank with one block and replace the lane with dry + modes.
     *
     * In place, because the lane IS the signal -- there is nowhere else for it
     * to go, and a scratch copy per block per instance is 1 KB of traffic for
     * no benefit.
     */
    void process(i64* l, i64* r, int n) noexcept {
        if (count == 0) return;
        for (int s = 0; s < n; ++s) {
            const i64 xl = l[s];
            const i64 xr = r[s];
            // THE GUARANTEE. A fixed-point two-pole does not decay to exact
            // zero, and the deadband below is not enough on its own: a slowly
            // decaying mode limit-cycles at roughly 1 / (4 (1 - r)) LSB -- for
            // a 200 ms mode at 95 Hz that is ~350, far outside it. Since an
            // instance stays alive while any mode rings, that was a slot that
            // was never reclaimed.
            //
            // So termination does not rest on the arithmetic. With no input for
            // twice a mode's declared decay, the mode is 120 dB down by its own
            // definition and is set to exact zero. It is pure integer counting,
            // so it happens on the same sample on every platform.
            if (xl == 0 && xr == 0) {
                if (quiet < 0xFFFFFFFFu) ++quiet;
            } else {
                quiet = 0;
            }
            i64 accl = msynth::asr(xl * dry_q16, GAIN_SHIFT);
            i64 accr = msynth::asr(xr * dry_q16, GAIN_SHIFT);
            for (int m = 0; m < count; ++m) {
                const ResonatorCoefs& c = coefs[m];
                if (c.gain == 0) continue;
                if (quiet >= limit[m]) {
                    z1l[m] = z2l[m] = z1r[m] = z2r[m] = 0;
                    continue;   // and it contributes nothing: exact silence
                }
                // The input, normalised and lifted into the state's extra
                // precision. i128 throughout: a lane value (up to ~2^43) times
                // a Q2.30 coefficient does not fit i64.
                const dnp::i128 nl = dnp::asr128(static_cast<dnp::i128>(xl) * c.norm,
                                                 30 - RESO_EXTRA);
                const dnp::i128 nr = dnp::asr128(static_cast<dnp::i128>(xr) * c.norm,
                                                 30 - RESO_EXTRA);
                // y = a1*y1 - a2*y2 + x, Q2.30 coefficients on scaled state.
                i64 yl = static_cast<i64>(
                    dnp::asr128(static_cast<dnp::i128>(c.a1) * z1l[m], 30)
                  - dnp::asr128(static_cast<dnp::i128>(c.a2) * z2l[m], 30) + nl);
                i64 yr = static_cast<i64>(
                    dnp::asr128(static_cast<dnp::i128>(c.a1) * z1r[m], 30)
                  - dnp::asr128(static_cast<dnp::i128>(c.a2) * z2r[m], 30) + nr);
                // A resonator is an IIR with a pole near the unit circle, so a
                // runaway is possible if a coefficient is ever wrong. Clamp the
                // STATE rather than the output: an unclamped state grows without
                // bound and the sound never recovers, where a clamped one
                // saturates and then decays like everything else.
                yl = clamp_state(yl);
                yr = clamp_state(yr);
                // The deadband, for the common case; the quiet counter above is
                // the guarantee. See DEADBAND.
                //
                // ONLY on silent input. Limit cycles are a zero-input
                // phenomenon, and a mode that is being excited starts from
                // nothing and grows -- checking every sample zeroed modes at
                // birth, so a long mode never rang at all.
                //
                // And on ENERGY, not on the instantaneous sample. A slow
                // sinusoid passes near zero at every crossing, so testing
                // |y| and |z1| cut a 40 Hz mode off ~6 dB before its declared
                // decay. The energy of a two-pole ring is invariant around the
                // cycle, so it falls only when the ring does.
                if (quiet > 0) {
                    if (ring_energy(c, yl, z1l[m]) < quiet_energy[m]) { yl = 0; z1l[m] = 0; }
                    if (ring_energy(c, yr, z1r[m]) < quiet_energy[m]) { yr = 0; z1r[m] = 0; }
                }
                z2l[m] = z1l[m]; z1l[m] = yl;
                z2r[m] = z1r[m]; z1r[m] = yr;
                // Back out of the extra precision, and apply the level.
                accl += static_cast<i64>(dnp::asr128(static_cast<dnp::i128>(yl) * c.gain,
                                                     GAIN_SHIFT + RESO_EXTRA));
                accr += static_cast<i64>(dnp::asr128(static_cast<dnp::i128>(yr) * c.gain,
                                                     GAIN_SHIFT + RESO_EXTRA));
            }
            l[s] = accl;
            r[s] = accr;
        }
    }

    /** Any mode still carrying energy, on either channel. */
    bool ringing() const noexcept {
        for (int m = 0; m < count; ++m)
            if (z1l[m] | z2l[m] | z1r[m] | z2r[m]) return true;
        return false;
    }

  private:
    // Well above any legitimate lane level (Q8.24 full scale is 1<<24), and
    // far below where i64 arithmetic would overflow in the multiply above.
    static constexpr i64 STATE_LIMIT = static_cast<i64>(1) << 58;   // scaled units

    // Inaudible by construction, not by judgement. The lane is Q8.24, so one
    // LSB of the 16-bit output is 1 << 9 = 512 at unity gain. The loudest a
    // mode may be is gain 4, so a state of 64 reaches the output as at most
    // 256: half an output LSB, below anything the int16 stage can represent.
    // Zeroing it cannot change a sample anyone receives by more than rounding
    // already does.
    static constexpr i64 DEADBAND = static_cast<i64>(64) << RESO_EXTRA;   // scaled units
    // E = y^2 - a1 y z1 + a2 z1^2, the invariant of the two-pole recursion:
    // for a ring y[n] = A r^n sin(wn + phi) it equals (A r^n sin w)^2 whatever
    // the phase. i128 throughout -- y reaches 2^58 in scaled units.
    static dnp::i128 ring_energy(const ResonatorCoefs& c, i64 y, i64 z1) noexcept {
        const dnp::i128 Y = y, Z = z1;
        return Y * Y - dnp::asr128(static_cast<dnp::i128>(c.a1) * Y, 30) * Z
                     + dnp::asr128(static_cast<dnp::i128>(c.a2) * Z, 30) * Z;
    }
    static i64 clamp_state(i64 v) noexcept {
        if (v > STATE_LIMIT) return STATE_LIMIT;
        if (v < -STATE_LIMIT) return -STATE_LIMIT;
        return v;
    }
};

}  // namespace dsnd
#endif  // DSND_RESONATOR_H
