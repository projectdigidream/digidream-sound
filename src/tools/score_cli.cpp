//
// dsound_score -- how close are two banks' versions of one role to the same
// reference?
//
//   dsound_score <reference.wav> <bankA.sfxproj> <bankB.sfxproj> <role>
//
// Prints two numbers: A's loss and B's, measured with exactly the loss the
// fitter minimises and over exactly the window it uses. That equality is the
// point -- a different window or a different weighting would produce numbers
// that look like a comparison and are not one.
//
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fit/fit.h"
#include "fit/wav_in.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_json.h"
#include "synth/json_min.h"
#include "synth/tables.h"

using namespace dsnd;

namespace {

bool read_file(const char* p, std::string& out) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.assign(static_cast<size_t>(n > 0 ? n : 0), '\0');
    const size_t got = out.empty() ? 0 : std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Shared with dsound_fit. Kept here rather than in a header because a WAV
// reader is not part of the library: the engine never loads audio.

// Render one named role of a bank, exactly as the cache would.
bool render_role(const std::string& bank_json, const std::string& role, size_t frames,
                 std::vector<float>& out) {
    WaveTableCache cache;
    SoundBank bank;
    if (!load_bank_json(bank_json.c_str(), bank_json.size(), cache, bank).ok) return false;
    const int id = bank.sound_index(role);
    if (id < 0) return false;

    std::vector<i32> pcm;
    u32 got = 0;
    const u32 max_blocks = static_cast<u32>((frames + CONTROL_BLOCK - 1) / CONTROL_BLOCK) + 1u;
    render_variant(bank, static_cast<size_t>(id), 0, /*mono=*/false, max_blocks, pcm, &got);

    out.assign(frames, 0.0f);
    const float scale = 1.0f / static_cast<float>(1 << SAMPLE_SHIFT);
    for (size_t i = 0; i < frames && i < got; ++i)
        out[i] = 0.5f * (static_cast<float>(pcm[i * 2]) + static_cast<float>(pcm[i * 2 + 1])) *
                 scale;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: dsound_score <reference.wav> <bankA.sfxproj> <bankB.sfxproj> "
                     "<role>\n");
        return 2;
    }
    msynth::tables_init();

    std::vector<float> target;
    float band = 0;
    if (!fit::read_reference(argv[1], target, &band) || target.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    std::string a_json, b_json;
    if (!read_file(argv[2], a_json) || !read_file(argv[3], b_json)) {
        std::fprintf(stderr, "cannot read a bank\n");
        return 1;
    }
    const std::string role = argv[4];

    // The same conditioning and the same window the fitter uses, so the two
    // numbers mean what the fitter's numbers mean.
    msynth::fit::prepare(target, static_cast<size_t>(1.6f * SAMPLE_RATE));
    size_t last = 0;
    for (size_t i = 0; i < target.size(); ++i)
        if (target[i] > 1e-4f || target[i] < -1e-4f) last = i;
    size_t win = last + SAMPLE_RATE / 20;
    if (win < 8192) win = 8192;
    if (win > target.size()) win = target.size();
    target.resize(win);

    std::vector<float> a, b;
    if (!render_role(a_json, role, win, a) || !render_role(b_json, role, win, b)) {
        std::fprintf(stderr, "role '%s' missing from a bank\n", role.c_str());
        return 1;
    }
    // THE measurement -- fit::score, the same band-limited loss the fitter
    // chose with. Used to be midisynth's spectral_loss, a second copy of the
    // arithmetic that happened to agree.
    const msynth::fit::LossWeights w{};
    std::printf("%.6f %.6f\n", static_cast<double>(fit::score(target, a, w, band)),
                static_cast<double>(fit::score(target, b, w, band)));
    return 0;
}
