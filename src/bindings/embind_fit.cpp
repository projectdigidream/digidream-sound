//
// The JavaScript face of the fitter. Compiled ONLY into dsound-tool.mjs.
//
// Why this is a separate translation unit with its own EMSCRIPTEN_BINDINGS
// block, rather than more methods on WebSfx: the engine module a game ships
// must not contain float DSP, an FFT, or libm, and the cleanest way to
// guarantee that is for the code not to be in the link at all.
//
// What keeps the two honest is scripts/run_goldens.sh, which renders every
// golden case through the native binary, through dsound.mjs, and through
// dsound-tool.mjs, and requires all three hashes to agree. So "adding the
// fitter changed nothing about the engine" is re-proved on every build rather
// than argued once.
//
// ONE RESTART PER CALL. fitOnce() does a single restart and returns; the
// worker loops and posts progress between calls. A fitter that ran to
// completion inside one call would freeze the worker for minutes with no way
// to show `restart k/N` and no way to cancel -- midisynth's fit-panel.js
// settled this shape already and there is no reason to differ.
//
// A NOTE ON WHAT THIS IS NOT. The engine is bit-identical between native and
// wasm, and the gate proves it. The FITTER IS NOT, and cannot be: the loss
// calls log, cos, pow, sqrt and polar, whose last bits differ between macOS
// libm and emscripten's musl. The loss differs, so the descent takes a
// different path, so the patches differ. That is fine -- a fitter is an
// authoring aid, and its output is a recipe a person then edits and hears --
// but do not let anyone write "a fit in the browser equals a fit on the CLI"
// into the docs, because it is not true.
//
#include <emscripten/bind.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "fit/fit.h"
#include "synth/tables.h"

using namespace dsnd;

namespace {

void esc(std::string& s, const std::string& t) {
    for (char c : t) {
        if (c == '"' || c == '\\') { s += '\\'; s += c; continue; }
        if (static_cast<unsigned char>(c) < 0x20) continue;
        s += c;
    }
}

// Enough digits to round-trip a float, without scientific notation, because
// the project's own JSON parser has no exponent form by design.
std::string fnum(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

}  // namespace

class SfxFitter {
  public:
    SfxFitter() { msynth::tables_init(); }

    // ---- the target ----

    // 48 kHz mono float, written into the heap by JS through HEAPF32.
    // prepare() then trims leading silence and normalises, exactly as
    // fit_best does, so what the optimiser sees here is what it sees there.
    // `bandHz` is the reference's bandwidth -- half its ORIGINAL sample rate,
    // before the browser resampled it to 48 kHz. Nothing above it is scored;
    // see BandLoss in fit.cpp.
    void setTarget(uintptr_t f32ptr, int len, float bandHz) {
        band_ = bandHz > 0 ? bandHz : 0.5f * SAMPLE_RATE;
        raw_.clear();
        if (len <= 0 || f32ptr == 0) return;
        const float* p = reinterpret_cast<const float*>(f32ptr);
        raw_.assign(p, p + len);
    }

    int targetFrames() const { return static_cast<int>(target_.size()); }

    // Copy the prepared target back out, for drawing it against a candidate.
    int targetTo(uintptr_t f32ptr, int maxLen) const {
        if (f32ptr == 0 || maxLen <= 0) return 0;
        float* o = reinterpret_cast<float*>(f32ptr);
        const int n = static_cast<int>(target_.size()) < maxLen
                    ? static_cast<int>(target_.size()) : maxLen;
        for (int i = 0; i < n; ++i) o[i] = target_[static_cast<size_t>(i)];
        return n;
    }

    // ---- architectures ----

    // The EXPANDED list -- twenty entries, not nine. `impact` is nine noise/
    // filter combinations and `blip` four falloff laws, and a browser dropdown
    // that showed only the families would hide two thirds of the search space
    // the fitter will try anyway.
    //
    // Shape: [{name, seconds, params:[{name, lo, hi, log}]}]
    std::string archListJson() const {
        std::string s = "[";
        bool first = true;
        for (const auto& e : fit::architectures_expanded()) {
            if (!first) s += ",";
            first = false;
            s += "{\"name\":\"";
            esc(s, e.name);
            s += "\",\"family\":\"";
            esc(s, e.base->name);
            s += "\",\"seconds\":" + fnum(e.base->seconds);
            s += ",\"params\":[";
            for (size_t i = 0; i < e.base->params.size(); ++i) {
                const auto& p = e.base->params[i];
                if (i) s += ",";
                s += "{\"name\":\"";
                esc(s, p.name);
                s += "\",\"lo\":" + fnum(p.lo);
                s += ",\"hi\":" + fnum(p.hi);
                s += ",\"log\":";
                s += p.log ? "true" : "false";
                s += "}";
            }
            s += "]}";
        }
        s += "]";
        return s;
    }

    // Choose an architecture and the loss weights, and size the analysis
    // window. Rebuilds the renderer, which is also the point at which its
    // wavetable cache is dropped -- a guaranteed reset between fits, so a long
    // session cannot accumulate.
    bool configure(const std::string& archName, float stft, float env, float bands,
                   float windowMs) {
        const auto all = fit::architectures_expanded();
        idx_ = -1;
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].name == archName) { idx_ = static_cast<int>(i); break; }
        if (idx_ < 0) return false;

        expanded_ = all[static_cast<size_t>(idx_)];
        weights_.stft = stft;
        weights_.env = env;
        weights_.bands = bands;

        const fit::Arch a = fit::instantiate(expanded_);
        const float secs = windowMs > 0 ? windowMs / 1000.0f : a.seconds;

        // Same window sizing as fit_best: prepare, find the real extent, add a
        // tail so a decay is compared rather than cut, and floor it at the
        // widest FFT the loss uses.
        target_ = raw_;
        fit::prepare(target_, static_cast<size_t>(secs * SAMPLE_RATE));
        size_t last = 0;
        for (size_t i = 0; i < target_.size(); ++i)
            if (target_[i] > 1e-4f || target_[i] < -1e-4f) last = i;
        size_t win = last + SAMPLE_RATE / 20;
        if (win < 8192) win = 8192;
        if (win > target_.size()) win = target_.size();
        target_.resize(win);

        rend_ = std::make_unique<fit::SfxRenderer>(win);
        // Tighter than the native default: this heap cannot grow, and an
        // abort inside a worker surfaces as an unhandled rejection with
        // -sASSERTIONS=0, which is close to no diagnostic at all.
        rend_->wt_cache_entries = 48;
        return true;
    }

    // ---- the fit ----

    // One restart. `seed` 0 starts every parameter at the centre of its range,
    // which is the best single guess; later seeds randomise.
    std::string fitOnce(int iters, int seed) {
        if (idx_ < 0 || !rend_) return "{\"error\":\"no architecture: call configure first\"}";
        if (target_.empty()) return "{\"error\":\"no target: call setTarget first\"}";

        const fit::Arch a = fit::instantiate(expanded_);
        float loss = 1e30f;
        const std::vector<float> u =
            // A negative seed asks for the restart begun on the target's own
            // spectral peaks, which fit_best always adds as an extra one.
            seed < 0
                ? fit::fit_once(a, target_, *rend_, iters, 0u, loss, weights_,
                                &(peak_start_ = fit::peak_seeded_start(a, target_, band_)), band_)
                : fit::fit_once(a, target_, *rend_, iters, static_cast<unsigned>(seed), loss,
                                weights_, nullptr, band_);

        std::string s = "{\"arch\":\"";
        esc(s, expanded_.name);
        s += "\",\"loss\":" + fnum(loss);
        s += ",\"finite\":";
        s += std::isfinite(loss) ? "true" : "false";

        // Normalised, so a caller can resume or perturb from here without
        // having to invert the architecture's ranges itself.
        s += ",\"u\":[";
        for (size_t i = 0; i < u.size(); ++i) { if (i) s += ","; s += fnum(u[i]); }
        s += "]";

        // Denormalised and named, which is what a knob panel wants.
        s += ",\"params\":{";
        for (size_t i = 0; i < u.size() && i < a.params.size(); ++i) {
            if (i) s += ",";
            s += "\"";
            esc(s, a.params[i].name);
            s += "\":" + fnum(fit::denorm(a.params[i], u[i]));
        }
        s += "}";

        s += ",\"patch\":";
        const std::string patch = fit::build_json(a, u);
        s += patch.empty() ? "null" : patch;
        s += "}";
        return s;
    }

    // Score a patch the user edited by hand against the same target and loss,
    // so "did my edit help" is answerable rather than a matter of opinion.
    std::string analyze(const std::string& patchJson) {
        if (!rend_ || target_.empty()) return "{\"error\":\"not configured\"}";
        std::vector<float> cand;
        if (!rend_->render(patchJson, cand)) return "{\"error\":\"the patch did not render\"}";
        const float l = fit::score(target_, cand, weights_, band_);
        return "{\"loss\":" + fnum(l) + "}";
    }

    // Render a patch into the heap for drawing. Float, not the engine's i16:
    // this is analysis, and it never reaches an output.
    int renderTo(const std::string& patchJson, uintptr_t f32ptr, int maxLen) {
        if (!rend_ || f32ptr == 0 || maxLen <= 0) return 0;
        std::vector<float> cand;
        if (!rend_->render(patchJson, cand)) return 0;
        float* o = reinterpret_cast<float*>(f32ptr);
        const int n = static_cast<int>(cand.size()) < maxLen
                    ? static_cast<int>(cand.size()) : maxLen;
        for (int i = 0; i < n; ++i) o[i] = cand[static_cast<size_t>(i)];
        return n;
    }

    // A recipe -> a patch, with no target and no fit: the editor's "Start
    // from" generators and its recipe sliders. `uJson` is the normalised
    // vector fitOnce() returns (each entry 0..1, clamped here), so a fitted
    // sound can be re-shaped by hand and a generated one can be fitted from.
    // Shorter vectors are padded with the centre of each range. Returns ""
    // for an unknown architecture.
    std::string buildPatch(const std::string& archName, const std::string& uJson) const {
        for (const auto& e : fit::architectures_expanded()) {
            if (e.name != archName) continue;
            const fit::Arch a = fit::instantiate(e);
            std::vector<float> u(a.params.size(), 0.5f);
            size_t i = 0;
            const char* p = uJson.c_str();
            while (*p && i < u.size()) {
                if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
                    char* end = nullptr;
                    const float v = std::strtof(p, &end);
                    if (end == p) { ++p; continue; }
                    u[i++] = v < 0 ? 0 : v > 1 ? 1 : v;
                    p = end;
                } else {
                    ++p;
                }
            }
            return fit::build_json(a, u);
        }
        return "";
    }

    // Check before a restart, not after an abort.
    int heapUsedBytes() const { return static_cast<int>(reinterpret_cast<uintptr_t>(sbrk(0))); }

  private:
    std::vector<float> raw_;      // as handed in
    float band_ = 0.5f * SAMPLE_RATE;
    std::vector<float> peak_start_;
    std::vector<float> target_;   // prepared and windowed
    fit::ExpandedArch expanded_;
    int idx_ = -1;
    fit::LossWeights weights_;
    std::unique_ptr<fit::SfxRenderer> rend_;
};

EMSCRIPTEN_BINDINGS(dsound_fit) {
    emscripten::class_<SfxFitter>("SfxFitter")
        .constructor<>()
        .function("setTarget", &SfxFitter::setTarget)
        .function("targetFrames", &SfxFitter::targetFrames)
        .function("targetTo", &SfxFitter::targetTo)
        .function("archListJson", &SfxFitter::archListJson)
        .function("configure", &SfxFitter::configure)
        .function("fitOnce", &SfxFitter::fitOnce)
        .function("analyze", &SfxFitter::analyze)
        .function("renderTo", &SfxFitter::renderTo)
        .function("buildPatch", &SfxFitter::buildPatch)
        .function("heapUsedBytes", &SfxFitter::heapUsedBytes);
}
