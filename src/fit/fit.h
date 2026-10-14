#pragma once
//
// Fitting: a reference sample in, a recipe out.
//
// midisynth already solved most of this (src/output/fit.h) and this file
// reuses it rather than reinventing it: the FFT, the multi-resolution
// log-spectrogram loss, the amplitude-envelope and octave-band terms, the
// parameter normalisation, the template substitution, and the target
// conditioning -- which already trims to the onset and normalises, so a fit
// is about timbre rather than about where in the file the sound starts.
//
// Two things are replaced, and both replacements are the point:
//
//   1. THE RENDERER. midisynth's renders a candidate through SynthEngine as a
//      one-note drum kit. This one renders it through the SFX path -- the same
//      render_variant() the PCM cache calls -- so what you fit is literally
//      what the game will play, and synth_engine.cpp stays out of this build.
//
//   2. THE ARCHITECTURES. midisynth's are musical: kick, snare, bass, piano,
//      pad, bell. None of them is a shotgun. Its own DAW doc warns that a
//      poor match "will be poor in a way the loss will not warn you about",
//      and a shotgun fitted to a `bell` architecture is exactly that case.
//      The list below is SFX-shaped, and it is the SAME list the content
//      generator uses as its families -- a preset is an architecture with its
//      knobs at a starting point, so one list serves both.
//
// This file is FLOAT code, and it lives outside the deterministic audio path
// exactly as midisynth's does. Nothing here is CALLED FROM a render: the arrow
// points one way, SfxRenderer::render -> render_variant, and never back.
//
// Note the precision of that claim. `scripts/build_wasm.sh` links this file
// into tool/dist/dsound-tool.mjs alongside the engine, so "nothing here is
// linked into a render" -- which this comment used to say -- is false. What
// keeps the property is reachability, not linkage, and run_goldens.sh proves
// it per build by requiring the engine-only and engine-plus-fitter modules to
// hash identically against the native binary.
//
#include <string>
#include <vector>

#include "output/fit.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_json.h"
#include "sfx/sound_def.h"

namespace dsnd {
namespace fit {

// The wavetable-cache ceiling, in distinct tables.
//
// An 8-frame table is 132 KiB (9 mips summing to 4224 i32 per frame), so this
// is a ceiling of about 12 MB. The figure is chosen to sit just above the
// largest working set a single restart actually needs, measured per
// architecture against a real reference:
//
//     voice 64   explosion 42   gunshot 31   impact 25   blip 1   whoosh 0
//
// Above the worst of those, so a restart never thrashes; bounded, so a long
// multi-restart run cannot grow without limit. The wasm fitter can tighten it
// through SfxRenderer::wt_cache_entries, because its heap is fixed.
inline constexpr u32 FIT_WT_CACHE_ENTRIES = 96;

using msynth::fit::Arch;
using msynth::fit::LossWeights;
using msynth::fit::Param;
using msynth::fit::build_json;
using msynth::fit::denorm;
using msynth::fit::prepare;
using msynth::fit::spectral_loss;

// Turn one fitted patch into a one-sound bank that the loader will accept.
//
// An architecture template produces a PATCH, because msynth::fit::Arch holds a
// single template string and is midisynth's struct, not ours to widen. But two
// things an architecture may need belong elsewhere:
//
//   "resonators", "resonatorDry"   the modal body, a property of the SOUND
//   "holdMs"                       a sustain's release, which is the VARIANT's
//                                  gesture: a note with that durMs
//   "gesture"                      several events -- the variant's gesture
//                                  itself, re-triggering the patch
//
// A template writes them inside its patch, and this lifts them to where they
// mean something. The patch loader ignores keys it does not know, so a patch
// that is NOT wrapped still loads -- it just silently loses its body, or is
// refused as a voice that can never end. That is why this is one function:
// the renderer, the tests and every tool that stores a fitted patch go through
// the same code rather than each re-deriving it.
//
// A non-spatial bus, so the render is stereo and untouched by distance or
// panning: a fit is of the sound, not its placement.
inline std::string wrap_as_bank(const std::string& patch_json) {
    std::string patch = patch_json;

    // Span a value from its ':' to the matching bracket (array/object) or to
    // the next top-level ',' or '}' (scalar), and remove "key": value.
    auto take = [&](const char* key, std::string& value) {
        const size_t k = patch.find(key);
        if (k == std::string::npos) return false;
        const size_t v = patch.find(':', k);
        if (v == std::string::npos) return false;
        size_t end = v + 1;
        int depth = 0;
        while (end < patch.size()) {
            const char c = patch[end];
            if (c == '[' || c == '{') ++depth;
            else if (c == ']' || c == '}') {
                if (depth == 0) break;
                --depth;
            } else if (c == ',' && depth == 0) break;
            ++end;
        }
        value = patch.substr(v + 1, end - v - 1);
        size_t cut = end;
        if (cut < patch.size() && patch[cut] == ',') ++cut;
        patch.erase(k, cut - k);
        return true;
    };

    std::string extras, value;
    if (take("\"resonators\"", value)) extras += "\"resonators\":" + value + ",";
    if (take("\"resonatorDry\"", value)) extras += "\"resonatorDry\":" + value + ",";
    // A whole GESTURE, for families that are several events rather than one
    // -- a splat is ten wet hits in a second, a pickup is two tones. It wins
    // over a hold: a family that re-triggers is not also sustaining.
    std::string gesture;
    if (take("\"gesture\"", value)) {
        gesture = "\"gesture\":" + value + ",";
        std::string ignored;
        take("\"holdMs\"", ignored);
    } else if (take("\"holdMs\"", value)) {
        gesture = "\"gesture\":[{\"atMs\":0,\"durMs\":" + value + "}],";
    }

    return "{\"version\":1,\"name\":\"fit\","
           "\"buses\":[{\"name\":\"b\",\"spatial\":false}],"
           "\"sounds\":[{\"name\":\"fit\"," + extras + "\"variants\":[{" + gesture +
           "\"patch\":" + patch + "}]}]}";
}

// Render a candidate patch through the SFX path and hand back float samples.
//
// The bank is rebuilt per candidate, which sounds wasteful and is not: the
// WaveTableCache is kept across candidates and dedupes by recipe, so a knob
// that does not touch the wavetable costs nothing to move.
struct SfxRenderer {
    WaveTableCache cache;
    SfxVoicePool pool;  // built once; see render_variant's note on why

    // The analysis window, in samples -- not the architecture's nominal
    // seconds. A pistol shot is 150 ms inside a 900 ms window, and the loss
    // is quadratic-ish in window length, so fitting the silence costs more
    // than fitting the sound. fit_best() sizes this from the target's actual
    // extent.
    size_t frames = SAMPLE_RATE;
    u32 rebuilds = 0;

    // How many distinct wavetables to hold before dropping the lot.
    //
    // This used to be a rebuild counter (`rebuilds % 4096`), which bounds the
    // wrong quantity: what costs memory is DISTINCT RECIPES, and most
    // parameters in an architecture -- gains, envelope times, filter cutoffs --
    // do not touch the wavetable at all, so rebuilds and tables differ by an
    // order of magnitude. A gunshot restart is 256 rebuilds and 31 tables.
    //
    // Be clear about the size of the problem, because the fix is small and the
    // temptation is to describe it as large: at realistic settings the old
    // counter effectively never fired, and a 3-restart fit peaks around 12 MB
    // either way. This is not a leak that was crashing anything. It is an
    // unbounded structure given a bound, in the quantity that actually grows,
    // before it is asked to run in a heap that cannot grow.
    //
    // It changes no fit result -- get_or_create regenerates a bit-identical
    // table from the same canonical recipe, the same property the PCM cache's
    // identity guarantee rests on -- and test_fit.cpp pins that rather than
    // asserting it.
    u32 wt_cache_entries = FIT_WT_CACHE_ENTRIES;

    explicit SfxRenderer(size_t n) : frames(n ? n : 1u) {}

    bool render(const std::string& patch_json, std::vector<float>& out) {
        out.clear();

        // A non-spatial bus, so the render is stereo and untouched by
        // distance or panning: we are fitting the sound, not its placement.
        //
        const std::string doc = wrap_as_bank(patch_json);

        SoundBank bank;
        LoadResult r = load_bank_json(doc.c_str(), doc.size(), cache, bank);
        if (!r.ok || bank.sounds.empty()) return false;
        ++rebuilds;

        // Drop the accumulated tables once they pass the bound. A long fit
        // explores thousands of wavetable recipes and the cache would
        // otherwise grow without limit.
        if (cache.size() >= wt_cache_entries) cache = WaveTableCache{};

        std::vector<i32> pcm;
        u32 got = 0;
        const u32 max_blocks =
            static_cast<u32>((frames + CONTROL_BLOCK - 1) / CONTROL_BLOCK) + 1u;
        render_variant(bank, 0, 0, /*mono=*/false, max_blocks, pcm, &got, nullptr, &pool);

        const size_t want = frames;
        out.assign(want, 0.0f);
        const float scale = 1.0f / static_cast<float>(1 << SAMPLE_SHIFT);
        for (size_t i = 0; i < want && i < got; ++i) {
            // Mono sum of the stereo render, so a fit is not confused by a
            // patch's own stereo image.
            const float l = static_cast<float>(pcm[i * 2]) * scale;
            const float rr = static_cast<float>(pcm[i * 2 + 1]) * scale;
            out[i] = 0.5f * (l + rr);
        }
        return true;
    }
};

// The SFX architectures. Structural choices are fixed here by hand, because
// they are exactly the ones a gradient cannot cross -- which oscillator, how
// many layers, which filter mode. Only the numbers move.
//
// Each carries the same shape as a family in content/game/make_doom2.py, and
// deliberately so: fitting a role produces a patch a person can then open in
// the editor and recognise, rather than a pile of numbers that happens to
// score well.
std::vector<Arch> architectures();

// The same list with its categorical choices spread out: `impact` becomes nine
// entries (three noise colours x three filter modes), `blip` four (its falloff
// law), the other seven stay as they are. Twenty in total.
//
// This exists because three things need to agree about what an architecture
// IS, and until now only fit_best could see the expanded form: the browser's
// dropdown, `dsound_fit --list`, and fit_best's own search space. A user who
// can only ask for "impact" cannot ask for the brown-noise highpass one that
// actually matches their door, even though the fitter will try it.
//
// The `%s` slots in the template are the categorical ones; `%g` slots remain
// the continuous parameters the optimiser moves.
struct ExpandedArch {
    std::string name;   // "impact/brown+highpass", or just "gunshot"
    std::string json;   // the template with every %s already substituted
    const Arch* base;   // params, seconds, note -- borrowed from architectures()
};

// NOTE the ownership, because getting it wrong is silent. `Arch::json` is a
// `const char*`, so an Arch whose json points into a temporary std::string
// dangles -- and a dangling template renders an empty patch, which fit_once
// scores as 1e30f, which reads as "that architecture never wins". A tuning
// mystery, not a crash. ExpandedArch therefore OWNS its json, and you call
// instantiate() only where the ExpandedArch outlives the Arch you get back.
std::vector<ExpandedArch> architectures_expanded();

// Borrow `e`'s template and parameters into an Arch the optimiser can take.
// The returned Arch is valid only while `e` lives.
Arch instantiate(const ExpandedArch& e);

// The target's strongest spectral peaks, ascending, in Hz.
//
// WHY THIS EXISTS. Finite-difference descent cannot place a resonant
// frequency. Move a mode by five per cent and it lands somewhere else in the
// spectrum entirely, so the loss surface in frequency is a field of spikes
// with nothing to follow between them -- a gradient method walks it blind.
// This is a known property of modal estimation, not a defect of this
// optimiser, and the standard answer is not a better descent but a better
// STARTING POINT: analyse the reference, find where it actually rings, and
// begin there.
//
// Used for every parameter whose name ends in "Hz", not only modal ones, since
// a filter cutoff sits on the same kind of surface.
std::vector<float> spectral_peaks(const std::vector<float>& target, size_t want,
                                  float band_hz = 0.5f * SAMPLE_RATE);

// One restart of finite-difference descent.
//
// The same algorithm as midisynth's fit_once -- it cannot be called directly
// because it is typed on midisynth's Renderer, and the renderer is the thing
// this project has to replace. The descent itself is unchanged.
//
// `start`, when given, is the normalised point to begin from; otherwise seed 0
// starts at the centre of every range and a non-zero seed randomises.
std::vector<float> fit_once(const Arch& arch, const std::vector<float>& target,
                            SfxRenderer& rend, int iters, unsigned seed, float& loss_out,
                            const LossWeights& weights = LossWeights{},
                            const std::vector<float>* start = nullptr,
                            float band_hz = 0.5f * SAMPLE_RATE);

// The target's strongest ONSETS, ascending, in ms from its start, not counting
// the one at zero.
//
// Time is the other axis descent cannot traverse. Moving an event from 100 to
// 300 ms does not slide its energy along a smooth slope; it vanishes from one
// place and appears in another, so the loss is spikes in time as it is in
// frequency. The same answer applies: find where the reference's events are,
// and start there.
std::vector<float> onset_times_ms(const std::vector<float>& target, size_t want);

// A starting vector with every "...Hz" parameter placed on a real peak of the
// target, every "...At" parameter on a real onset, and everything else at the
// centre of its range.
std::vector<float> peak_seeded_start(const Arch& arch, const std::vector<float>& target,
                                     float band_hz = 0.5f * SAMPLE_RATE);

// Fit a target against every architecture and keep the best. `restarts` is
// per architecture.
struct FitResult {
    std::string arch;
    std::string patch_json;
    float loss = 1e30f;
    std::vector<float> params;

    // Why nothing came back, when nothing came back. A fitter that returns an
    // empty result and says nothing is the worst possible failure mode: it
    // looks like "no good match" when it is actually a bug.
    int restarts_run = 0;
    int non_finite = 0;   // restarts whose loss was NaN or infinite
    float worst = 0;      // the largest finite loss seen
};

FitResult fit_best(const std::vector<float>& target, float seconds, int restarts, int iters,
                   const LossWeights& weights = LossWeights{},
                   const std::string& only_arch = "",
                   float band_hz = 0.5f * SAMPLE_RATE);

// THE measurement. One band-limited loss, used by the fitter, dsound_score,
// keep_better.py and the browser alike, so a fit is chosen and judged by the
// same arithmetic. `band_hz` is the reference's bandwidth -- half its original
// sample rate; see wav_in.h. Bins above it are not scored.
float score(const std::vector<float>& target, const std::vector<float>& candidate,
            const LossWeights& weights, float band_hz);

}  // namespace fit
}  // namespace dsnd
