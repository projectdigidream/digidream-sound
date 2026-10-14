//
// The fitter.
//
// A fitter is hard to test because "does it sound right" is a matter of
// taste. A SELF-FIT is not: render a recipe the fitter could have produced,
// hand the audio back as a target, and the descent must recover something
// close to it. If it cannot match a sound its own architecture generated, it
// certainly cannot match a recording.
//
// Kept deliberately small -- one architecture, few restarts -- because the
// unit suite runs on every build and a full fit is seconds, not milliseconds.
//
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "fit/fit.h"
#include "fit/wav_in.h"
#include "test_framework.h"

using namespace dsnd;

namespace {

const fit::Arch* find_arch(const std::vector<fit::Arch>& all, const char* name) {
    for (const auto& a : all)
        if (std::string(a.name) == name) return &a;
    return nullptr;
}

// Render one architecture at a chosen point in its parameter space.
std::vector<float> render_at(const fit::Arch& a, const std::vector<float>& u, size_t frames) {
    fit::SfxRenderer rend(frames);
    std::vector<float> out;
    const std::string js = fit::build_json(a, u);
    CHECK(!js.empty());
    CHECK(rend.render(js, out));
    return out;
}

}  // namespace

TEST(fit_architectures_are_well_formed) {
    const auto all = fit::architectures();
    CHECK(all.size() >= 8);
    for (const auto& a : all) {
        CHECK(a.params.size() > 0);
        CHECK(a.seconds > 0);
        // Every %g must have a parameter, and every parameter a %g -- a
        // mismatch silently produces an empty patch, which the optimiser
        // then scores as infinitely bad for the whole architecture.
        std::vector<float> u(a.params.size(), 0.5f);
        const std::string js = fit::build_json(a, u);
        CHECK(!js.empty());
        // And it must actually load.
        WaveTableCache cache;
        SoundBank bank;
        // Through wrap_as_bank, the same path every tool that stores a fitted
        // patch uses -- a patch with a hold or a body is only valid once its
        // sound-level and variant-level fields have been lifted out.
        const std::string doc = fit::wrap_as_bank(js);
        LoadResult r = load_bank_json(doc.c_str(), doc.size(), cache, bank);
        if (!r.ok)
            for (const auto& e : r.errors) std::printf("  %s: %s\n", a.name, e.c_str());
        CHECK(r.ok);
    }
}

TEST(fit_every_architecture_makes_a_sound_at_its_centre) {
    // A silent architecture is one nobody will ever fit anything with, and
    // the loss will not say so -- it will just never be chosen.
    for (const auto& a : fit::architectures()) {
        std::vector<float> u(a.params.size(), 0.5f);
        const auto got = render_at(a, u, 16384);
        float peak = 0;
        for (float v : got) peak = std::max(peak, std::fabs(v));
        if (peak <= 1e-4f) std::printf("  silent architecture: %s\n", a.name);
        CHECK(peak > 1e-4f);
    }
}

// The self-fit. `whoosh` has seven parameters, so a short descent is enough
// to show the machinery works end to end.
TEST(fit_recovers_a_recipe_it_generated) {
    const auto all = fit::architectures();
    const fit::Arch* a = find_arch(all, "whoosh");
    CHECK(a != nullptr);
    if (!a) return;

    // A target well away from the centre, so starting at the centre is not
    // already the answer.
    std::vector<float> truth(a->params.size());
    for (size_t i = 0; i < truth.size(); ++i) truth[i] = (i % 2 == 0) ? 0.78f : 0.22f;

    const size_t win = 16384;
    const auto target = render_at(*a, truth, win);

    fit::SfxRenderer rend(win);

    // The loss at the centre, before any descent -- the number to beat.
    float before = 0;
    {
        std::vector<float> u(a->params.size(), 0.5f);
        std::vector<float> got;
        CHECK(rend.render(fit::build_json(*a, u), got));
        // The fitter's own measurement, so before and after are comparable.
        before = fit::score(target, got, fit::LossWeights{}, 0.5f * SAMPLE_RATE);
    }

    float after = 1e30f;
    const std::vector<float> u = fit::fit_once(*a, target, rend, 40, 0, after);
    CHECK_EQ(u.size(), a->params.size());

    // It must get substantially closer. Not exact: the descent is
    // finite-difference on a rough surface and several parameter
    // combinations sound alike, which is a property of the instrument rather
    // than a defect of the search.
    std::printf("  whoosh self-fit: %.4f -> %.4f\n", static_cast<double>(before),
                static_cast<double>(after));
    CHECK(after < before);
    CHECK(after < before * 0.6f);
}

// Fitting a target that IS one of the architectures should pick that
// architecture, not another one that happens to score well on a bad day.
TEST(fit_best_picks_a_plausible_architecture) {
    const auto all = fit::architectures();
    const fit::Arch* a = find_arch(all, "blip");
    CHECK(a != nullptr);
    if (!a) return;

    std::vector<float> truth(a->params.size(), 0.45f);
    const auto target = render_at(*a, truth, 16384);

    // Only two architectures, so the test stays under a second: the right
    // one and a deliberately wrong one.
    const fit::FitResult blip = fit::fit_best(target, 0.34f, 1, 12, fit::LossWeights{}, "blip");
    const fit::FitResult whoosh = fit::fit_best(target, 0.34f, 1, 12, fit::LossWeights{}, "whoosh");
    std::printf("  blip %.4f vs whoosh %.4f\n", static_cast<double>(blip.loss),
                static_cast<double>(whoosh.loss));
    CHECK(blip.loss < whoosh.loss);
    // The result names the EXPANSION that won -- "blip/saw", not "blip" --
    // because the family alone is not something you can reproduce. Asking for
    // the family still fits all four of them.
    CHECK(blip.arch.rfind("blip", 0) == 0);
    CHECK(!blip.patch_json.empty());
}

// The wavetable-cache bound must not change what the fitter finds.
//
// SfxRenderer drops its WaveTableCache once it holds wt_cache_entries distinct
// tables, which is what keeps a browser fit inside a heap that cannot grow.
// That is only safe because get_or_create rebuilds a bit-identical table from
// the same canonical recipe -- the cache is a speed optimisation and never a
// source of state. If that were ever false, a fit would depend on how much
// memory happened to be free, which is the same class of bug as seeding a
// voice from its pool slot.
//
// So: fit the same target twice, once with a cache that holds essentially
// nothing and once with one that never drops, and require bit equality.
TEST(fit_cache_bound_does_not_change_the_result) {
    const auto all = fit::architectures();
    const fit::Arch* a = find_arch(all, "whoosh");  // 7 params: the quickest
    CHECK(a != nullptr);
    if (!a) return;

    std::vector<float> truth(a->params.size(), 0.4f);
    const auto target = render_at(*a, truth, 16384);

    float loss_tiny = 0, loss_huge = 0;
    fit::SfxRenderer tiny(16384);
    tiny.wt_cache_entries = 1;  // drop on every rebuild
    const auto u_tiny = fit::fit_once(*a, target, tiny, 8, 1234u, loss_tiny);

    fit::SfxRenderer huge(16384);
    huge.wt_cache_entries = 1u << 30;  // never drop
    const auto u_huge = fit::fit_once(*a, target, huge, 8, 1234u, loss_huge);

    CHECK(u_tiny.size() == u_huge.size());
    bool same = u_tiny.size() == u_huge.size();
    for (size_t i = 0; i < u_tiny.size() && same; ++i)
        if (u_tiny[i] != u_huge[i]) same = false;
    CHECK(same);
    CHECK(loss_tiny == loss_huge);
    std::printf("  loss %.6f both ways, %zu params bit-equal\n",
                static_cast<double>(loss_tiny), u_tiny.size());
}

// The expanded architecture list is what the CLI and the browser both show.
//
// Until it existed, only fit_best could see the categorical expansions, so a
// user could ask for "impact" but never for the brown-noise highpass variant
// the fitter would happily try. Three consumers now read one list; this pins
// its shape and, more importantly, the ownership rule in its declaration.
TEST(architectures_expanded_is_one_list_for_everyone) {
    const auto all = fit::architectures_expanded();
    // 10 plain + impact x9 + blip x4 + modal x3 + roar x2 + burst x2 + sequence x2 = 32.
    //
    // Two joined the list for reasons worth naming. `weapon` is the first
    // architecture that places DELAYED layers, which is what a firearm with a
    // mechanical action needs and what none of the others could express.
    // `modal` is the first that is not subtractive at all: a short exciter
    // ringing a bank of damped resonators, which is how the literature builds
    // anything struck.
    CHECK(all.size() == 32);

    int impacts = 0, blips = 0, modals = 0;
    for (const auto& e : all) {
        CHECK(e.base != nullptr);
        if (!e.base) continue;
        if (std::string(e.base->name) == "impact") ++impacts;
        if (std::string(e.base->name) == "blip") ++blips;
        if (std::string(e.base->name) == "modal") ++modals;

        // No template may still carry an unfilled categorical slot: a stray
        // %s reaches load_patch as a literal and the patch silently fails.
        CHECK(e.json.find("%s") == std::string::npos);

        // The borrowed pointer must survive the call -- this is the dangling
        // const char* the header warns about, checked rather than trusted.
        const fit::Arch a = fit::instantiate(e);
        CHECK(std::string(a.json) == e.json);
        CHECK(a.params.size() == e.base->params.size());
    }
    CHECK(impacts == 9);
    CHECK(blips == 4);
    CHECK(modals == 3);

    // A family name still selects every expansion of it, so --arch impact
    // keeps meaning what it always meant.
    int matched = 0;
    for (const auto& e : all)
        if (std::string(e.base->name) == "impact") ++matched;
    CHECK(matched == 9);
}

// Spectral peak picking, and the starting point built from it.
//
// Finite differences cannot traverse a frequency axis: move a resonant mode by
// five per cent and it lands somewhere else entirely, so the loss surface in
// frequency is spikes with nothing to follow between them. The fix is a better
// STARTING POINT rather than a better step -- analyse the reference, find where
// it actually rings, and begin there.
//
// The first version of this REPLACED the centre start and measured worse on
// four of six references, because a point with frequencies at their extremes
// and everything else in the middle sits in a poorer basin than a neutral one.
// It is now an EXTRA restart, which cannot regress because fit_best keeps the
// best of all of them. This pins the analysis; the non-regression is
// structural.
TEST(spectral_peaks_finds_the_partials_that_are_there) {
    // Three sinusoids at known frequencies, in a window long enough for the
    // 4096-point analysis to resolve them.
    const size_t n = 48000;
    std::vector<float> x(n, 0.0f);
    const float want[3] = {440.0f, 1200.0f, 3000.0f};
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        for (float f : want)
            x[i] += std::sin(2.0f * 3.14159265f * f * t) / 3.0f;
    }

    const auto peaks = fit::spectral_peaks(x, 3);
    CHECK(peaks.size() == 3);
    if (peaks.size() != 3) return;
    // Ascending, and within one bin of the truth. A 4096-point window at
    // 48 kHz is 11.7 Hz per bin, so the tolerance is the resolution.
    for (int i = 0; i < 3; ++i) {
        std::printf("  peak %d: %.0f Hz (want %.0f)\n", i, static_cast<double>(peaks[i]),
                    static_cast<double>(want[i]));
        CHECK(std::fabs(peaks[i] - want[i]) < 25.0f);
    }
}

TEST(a_peak_seeded_start_puts_frequency_parameters_on_peaks) {
    const auto all = fit::architectures();
    const fit::Arch* a = find_arch(all, "modal");
    CHECK(a != nullptr);
    if (!a) return;

    const size_t n = 48000;
    std::vector<float> x(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        x[i] = 0.5f * std::sin(2.0f * 3.14159265f * 300.0f * t)
             + 0.5f * std::sin(2.0f * 3.14159265f * 2000.0f * t);
    }

    const auto u = fit::peak_seeded_start(*a, x);
    CHECK(u.size() == a->params.size());
    if (u.size() != a->params.size()) return;

    // Every parameter is in range, and the ones that are NOT frequencies are
    // left at the centre -- moving them would be guessing.
    bool any_moved = false;
    for (size_t i = 0; i < u.size(); ++i) {
        CHECK(u[i] >= 0.0f && u[i] <= 1.0f);
        const std::string nm = a->params[i].name;
        const bool is_hz = nm.size() > 2 && nm.compare(nm.size() - 2, 2, "Hz") == 0;
        if (!is_hz) CHECK(u[i] == 0.5f);
        else if (u[i] != 0.5f) any_moved = true;
    }
    CHECK(any_moved);

    // And a target with nothing in it must not invent peaks: a start of all
    // centres is the honest answer, not a set of arbitrary frequencies.
    std::vector<float> silence(n, 0.0f);
    const auto flat = fit::peak_seeded_start(*a, silence);
    for (float v : flat) CHECK(v == 0.5f);
}

// ---------------------------------------------------------------------------
// The measurement.
//
// Scored on the DOOM set, about 80% of the distance the fitter used to
// minimise came from above 5.5 kHz, where an 11 kHz reference contains only a
// resampler's images and 8-bit hiss. These pin the three properties that stop
// that recurring: nothing above the band is scored, nothing below the noise
// floor is scored, and the resampler adds no images to score in the first
// place.
// ---------------------------------------------------------------------------

namespace {
std::vector<float> tone(float hz, float amp, size_t n, float phase = 0) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = amp * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i) / 48000.0f + phase);
    return x;
}
std::vector<float> plus(std::vector<float> a, const std::vector<float>& b) {
    for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
    return a;
}
}  // namespace

TEST(the_loss_ignores_everything_above_the_references_band) {
    const size_t n = 24000;
    const auto target = plus(tone(440, 0.5f, n), tone(2000, 0.2f, n));
    const float band = 5512.5f;   // an 11025 Hz reference
    const fit::LossWeights w{};

    // Identical below the band, wildly different above it: the same score.
    const auto clean = target;
    const auto fizzy = plus(target, tone(9000, 0.4f, n));
    const float a = fit::score(target, clean, w, band);
    const float b = fit::score(target, fizzy, w, band);
    std::printf("  in-band identical: clean %.4f, +9 kHz fizz %.4f\n",
                static_cast<double>(a), static_cast<double>(b));
    CHECK(std::fabs(a - b) < 0.02f);

    // But different BELOW the band must still count, or the loss is blind.
    const auto wrong = plus(tone(440, 0.5f, n), tone(3100, 0.2f, n));
    const float c = fit::score(target, wrong, w, band);
    CHECK(c > a + 0.1f);

    // And the full band is still available for a full-band reference.
    CHECK(fit::score(target, fizzy, w, 24000.0f) > a + 0.1f);
}

TEST(the_loss_ignores_what_lies_below_the_references_noise_floor) {
    // A reference that decays into hiss 70 dB down, against a candidate that
    // decays to true silence: equally right, since the hiss is the recording
    // medium and not the sound.
    const size_t n = 24000;
    auto target = tone(600, 0.8f, n);
    auto hiss = target;
    unsigned s = 12345;
    for (float& v : hiss) {
        s = s * 1664525u + 1013904223u;
        v += 0.8f * 3.2e-4f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);   // ~-70 dB
    }
    const fit::LossWeights w{};
    const float with_hiss = fit::score(hiss, hiss, w, 24000.0f);
    const float silent = fit::score(hiss, target, w, 24000.0f);
    std::printf("  against a hissy reference: its own hiss %.4f, clean %.4f\n",
                static_cast<double>(with_hiss), static_cast<double>(silent));
    CHECK(silent - with_hiss < 0.05f);
}

TEST(the_reference_resampler_adds_no_images) {
    // A 3 kHz tone at 11025 Hz, upsampled. Repeating samples -- what the old
    // reader did -- puts images at 11025 +/- 3000 and above; a band-limited
    // resampler must leave essentially nothing above the old Nyquist.
    const int rate = 11025;
    std::vector<float> in(rate);   // one second
    for (int i = 0; i < rate; ++i)
        in[static_cast<size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265f * 3000.0f * static_cast<float>(i) / rate);
    const auto out = fit::resample_to_engine(in, rate);
    CHECK(out.size() > 47000 && out.size() < 49000);

    // Energy above 5.6 kHz as a fraction of the total, over the middle of the
    // signal (the ends see a truncated kernel).
    const size_t N = 8192, off = out.size() / 2 - N / 2;
    std::vector<std::complex<float>> buf(N);
    for (size_t i = 0; i < N; ++i) {
        const float wv = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / N);
        buf[i] = {out[off + i] * wv, 0.0f};
    }
    msynth::fit::fft(buf);
    double lo = 0, hi = 0;
    for (size_t k = 0; k < N / 2; ++k) {
        const double e = std::norm(buf[k]);
        (k * 48000.0 / N > 5600.0 ? hi : lo) += e;
    }
    const double share = hi / (lo + hi);
    std::printf("  energy above 5.6 kHz after resampling: %.2e of total\n", share);
    CHECK(share < 1e-4);
}

// Onsets: time is the other axis descent cannot cross.
//
// Moving an event from 100 to 300 ms does not slide its energy along a slope;
// it vanishes from one place and appears in another. So `...At` parameters
// start on the reference's detected onsets, the way `...Hz` ones start on its
// spectral peaks -- and `burst` and `sequence` are built on that.
TEST(onset_times_finds_events_where_they_are) {
    const size_t n = 48000;
    std::vector<float> x(n, 0.0f);
    const float at_ms[] = {0.0f, 150.0f, 420.0f, 700.0f};
    unsigned s = 7;
    for (float ms : at_ms) {
        const size_t a = static_cast<size_t>(ms * 48.0f);
        for (size_t i = 0; i < 1500 && a + i < n; ++i) {   // a 30 ms noise click
            s = s * 1664525u + 1013904223u;
            const float noise = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            x[a + i] += 0.6f * noise * std::exp(-static_cast<float>(i) / 300.0f);
        }
    }
    const auto on = fit::onset_times_ms(x, 8);
    std::printf("  onsets:");
    for (float v : on) std::printf(" %.0f", static_cast<double>(v));
    std::printf(" ms (want 150 420 700)\n");
    CHECK(on.size() == 3);   // the event at zero is not a LATER event
    if (on.size() != 3) return;
    // Within one 5 ms frame. The first version was 15 ms early on every one,
    // and a tolerance of 15 let it pass.
    for (int i = 0; i < 3; ++i) CHECK(std::fabs(on[i] - at_ms[i + 1]) <= 5.0f);

    // And a steady tone has none: a swell or a hold is not an event.
    const auto flat = fit::onset_times_ms(tone(300, 0.5f, n), 8);
    CHECK(flat.empty());
}
