#pragma once
//
// Reading a reference recording, for the fitter and the scorer alike.
//
// ONE reader, because there used to be two -- fit_cli.cpp and score_cli.cpp
// each carried a copy -- and the fitter and the scorer have to agree exactly
// about what a reference IS, or keep_better.py ends up choosing between fits
// by a different measurement from the one that produced them.
//
// It also reports the reference's BANDWIDTH, which the loss now needs: a
// DOOM lump is 11025 Hz, so it has nothing above 5.5 kHz, and scoring a fit
// on bins where the reference has no information is scoring noise.
//
// RESAMPLING IS A WINDOWED SINC, not nearest-neighbour. The old reader
// repeated samples, on the argument that an 11 kHz reference "carries no
// content above 5.5 kHz for a better resampler to preserve". That is true and
// beside the point: a staircase does not merely fail to add content above the
// old Nyquist, it CREATES it -- mirror images of the whole spectrum,
// attenuated only by a sinc. Measured on the DOOM set, 5% of the target's
// energy on average (14% for the chainsaw) was those images, and because the
// loss compares LOG spectra, where every bin counts equally regardless of how
// little energy it holds, they were roughly 80% of the distance the fitter
// was minimising. It was spending most of its effort trying to reproduce
// aliasing.
//
// Float code, like everything under src/fit/: it runs on the reference once,
// before fitting, and never in a render.
//
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sfx/config.h"

namespace dsnd {
namespace fit {

// Resample `in` from `rate` to the engine's SAMPLE_RATE with a Hann-windowed
// sinc. The cutoff is the lower of the two Nyquists, so upsampling adds no
// images and downsampling adds no aliases.
inline std::vector<float> resample_to_engine(const std::vector<float>& in, int rate) {
    if (rate == static_cast<int>(SAMPLE_RATE) || rate <= 0 || in.empty()) return in;
    const double ratio = static_cast<double>(rate) / SAMPLE_RATE;   // input samples per output
    const double fc = ratio < 1.0 ? 1.0 : 1.0 / ratio;              // cutoff, in input-sample units
    const int H = 24;                                                // half-width, input samples
    const double pi = 3.14159265358979323846;
    const size_t n_out = static_cast<size_t>(static_cast<double>(in.size()) / ratio);
    std::vector<float> out(n_out, 0.0f);
    const int span = static_cast<int>(std::ceil(H / fc));
    for (size_t i = 0; i < n_out; ++i) {
        const double p = static_cast<double>(i) * ratio;
        const long c = static_cast<long>(std::floor(p));
        double acc = 0, wsum = 0;
        for (long k = c - span + 1; k <= c + span; ++k) {
            if (k < 0 || k >= static_cast<long>(in.size())) continue;
            const double d = p - static_cast<double>(k);
            const double x = d * fc;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(pi * x) / (pi * x);
            const double w = 0.5 + 0.5 * std::cos(pi * d / span);
            const double h = fc * sinc * w;
            acc += h * in[static_cast<size_t>(k)];
            wsum += h;
        }
        // Normalising by the kernel's own sum keeps DC exact near the edges,
        // where the window is truncated.
        out[i] = static_cast<float>(wsum > 1e-12 ? acc / wsum : acc);
    }
    return out;
}

// Read a 16-bit PCM WAV, mix to mono, resample to the engine rate. On success
// `band_hz` is the highest frequency the recording can contain: half its
// original sample rate, capped at the engine's own Nyquist.
inline bool read_reference(const char* path, std::vector<float>& out, float* band_hz = nullptr,
                           int* rate_out = nullptr) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> d(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = d.empty() ? 0 : std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if (got != d.size() || d.size() < 44) return false;
    if (std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;

    auto rd16 = [&](size_t at) { return static_cast<unsigned>(d[at] | (d[at + 1] << 8)); };
    auto rd32 = [&](size_t at) {
        return static_cast<unsigned>(d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) |
                                     (static_cast<unsigned>(d[at + 3]) << 24));
    };

    size_t at = 12;
    int channels = 1, rate = static_cast<int>(SAMPLE_RATE), bits = 16;
    const unsigned char* data = nullptr;
    size_t data_len = 0;
    while (at + 8 <= d.size()) {
        const unsigned sz = rd32(at + 4);
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0 && at + 8 + 16 <= d.size()) {
            channels = static_cast<int>(rd16(at + 10));
            rate = static_cast<int>(rd32(at + 12));
            bits = static_cast<int>(rd16(at + 22));
        } else if (std::memcmp(d.data() + at, "data", 4) == 0) {
            data = d.data() + at + 8;
            data_len = std::min<size_t>(sz, d.size() - at - 8);
            break;
        }
        at += 8 + sz + (sz & 1);
    }
    if (!data || bits != 16 || channels < 1) return false;

    const size_t frames = data_len / (2u * static_cast<size_t>(channels));
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        int acc = 0;
        for (int c = 0; c < channels; ++c) {
            const size_t k = (i * static_cast<size_t>(channels) + static_cast<size_t>(c)) * 2;
            acc += static_cast<short>(d[k + (data - d.data())] | (d[k + 1 + (data - d.data())] << 8));
        }
        mono[i] = static_cast<float>(acc) / (32768.0f * static_cast<float>(channels));
    }

    out = resample_to_engine(mono, rate);
    if (band_hz) {
        const float nyq = 0.5f * static_cast<float>(rate > 0 ? rate : SAMPLE_RATE);
        *band_hz = nyq < 0.5f * SAMPLE_RATE ? nyq : 0.5f * SAMPLE_RATE;
    }
    if (rate_out) *rate_out = rate;
    return true;
}

}  // namespace fit
}  // namespace dsnd
