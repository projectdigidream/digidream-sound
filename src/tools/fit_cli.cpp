//
// dsound_fit -- a reference sample in, a recipe out.
//
//   dsound_fit <reference.wav> [options]
//     --arch NAME       fit only this architecture (default: try them all)
//     --restarts N      restarts per architecture (default 3)
//     --iters N         descent steps per restart (default 60)
//     --seconds S       window to fit (default: the architecture's own)
//     --out FILE        write the fitted patch JSON here
//     --wav FILE        render the fitted patch here, to compare by ear
//     --list            print the architectures and stop
//     --quiet
//
// The architecture is chosen structurally, not by the optimiser: oscillator
// type, layer count and filter mode are exactly the choices a gradient cannot
// cross, so each one is fitted separately and the best result wins. That is
// also why the output is a patch a person can open and recognise rather than
// an opaque parameter vector.
//
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fit/fit.h"
#include "fit/wav_in.h"
#include "output/wav_writer.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_json.h"
#include "synth/tables.h"

using namespace dsnd;

namespace {

// The reference reader lives in fit/wav_in.h, shared with dsound_score so the
// fitter and the scorer agree exactly about what a reference is.

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: dsound_fit <reference.wav> [--arch NAME] [--restarts N] "
                     "[--iters N]\n"
                     "                  [--seconds S] [--out patch.json] [--wav out.wav] "
                     "[--list] [--quiet]\n");
        return 2;
    }
    msynth::tables_init();

    if (std::string(argv[1]) == "--list") {
        // The expanded list, because --arch accepts either form and a menu
        // that hides half the search space is a menu that lies. `impact` is
        // nine entries and `blip` four; passing the family name still fits
        // every expansion of it.
        for (const auto& e : fit::architectures_expanded())
            std::printf("%-26s %zu params, %.1fs window\n", e.name.c_str(),
                        e.base->params.size(), static_cast<double>(e.base->seconds));
        return 0;
    }

    const char* ref_path = argv[1];
    std::string only_arch, out_json, out_wav;
    int restarts = 3, iters = 60;
    float seconds = 0;
    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arch" && i + 1 < argc) only_arch = argv[++i];
        else if (a == "--restarts" && i + 1 < argc) restarts = atoi(argv[++i]);
        else if (a == "--iters" && i + 1 < argc) iters = atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = static_cast<float>(atof(argv[++i]));
        else if (a == "--out" && i + 1 < argc) out_json = argv[++i];
        else if (a == "--wav" && i + 1 < argc) out_wav = argv[++i];
        else if (a == "--quiet") quiet = true;
        else {
            std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
            return 2;
        }
    }

    std::vector<float> target;
    int rate = 0;
    float band = 0;
    if (!fit::read_reference(ref_path, target, &band, &rate)) {
        std::fprintf(stderr, "cannot read %s (16-bit PCM WAV expected)\n", ref_path);
        return 1;
    }
    if (target.empty()) {
        std::fprintf(stderr, "%s is empty\n", ref_path);
        return 1;
    }
    if (!quiet)
        std::fprintf(stderr, "%s: %zu frames at %d Hz, scored up to %.0f Hz\n", ref_path,
                     target.size(), rate, static_cast<double>(band));

    const fit::FitResult best =
        fit::fit_best(target, seconds, restarts, iters, msynth::fit::LossWeights{}, only_arch,
                      band);

    if (best.patch_json.empty()) {
        std::fprintf(stderr,
                     "no architecture produced a patch: %d restarts run, %d of them "
                     "scored non-finite.\n",
                     best.restarts_run, best.non_finite);
        if (best.restarts_run == 0)
            std::fprintf(stderr, "  (no architecture matched --arch '%s')\n", only_arch.c_str());
        return 1;
    }
    if (!quiet) std::fprintf(stderr, "best: %s  loss %.4f\n", best.arch.c_str(),
                             static_cast<double>(best.loss));

    if (!out_json.empty()) {
        FILE* f = std::fopen(out_json.c_str(), "wb");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", out_json.c_str());
            return 1;
        }
        std::fwrite(best.patch_json.data(), 1, best.patch_json.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    } else {
        std::printf("%s\n", best.patch_json.c_str());
    }

    if (!out_wav.empty()) {
        // Render the fit through the same path the game would, so what you
        // compare by ear is what would actually ship.
        const std::string doc =
            "{\"version\":1,\"name\":\"fit\","
            "\"buses\":[{\"name\":\"b\",\"spatial\":false}],"
            "\"sounds\":[{\"name\":\"fit\",\"variants\":[{\"patch\":" +
            best.patch_json + "}]}]}";
        WaveTableCache cache;
        SoundBank bank;
        if (load_bank_json(doc.c_str(), doc.size(), cache, bank).ok) {
            std::vector<i32> pcm;
            u32 frames = 0;
            render_variant(bank, 0, 0, false, 4 * CONTROL_RATE, pcm, &frames);
            std::vector<i16> out(pcm.size());
            for (size_t i = 0; i < pcm.size(); ++i) {
                i64 v = msynth::asr(static_cast<i64>(pcm[i]), SAMPLE_SHIFT - 15);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                out[i] = static_cast<i16>(v);
            }
            msynth::write_wav16(out_wav, out, SAMPLE_RATE, 2);
            if (!quiet) std::fprintf(stderr, "wrote %s\n", out_wav.c_str());
        }
    }
    return 0;
}
