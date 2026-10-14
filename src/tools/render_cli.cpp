//
// dsound_render -- play a .sfxscript through the engine, offline, into a WAV.
//
// This is the determinism gate's front door. The same bank and the same script
// must produce the same sha256 on every target, and scripts/run_goldens.sh is
// what holds the build to that.
//
//   dsound_render <bank.sfxb|bank.sfxproj> <script.sfxscript> <out.wav>
//                 [--hash] [--no-cache] [--budget-kb N] [--seconds N] [--quiet]
//
// --no-cache forces every sound to synthesize live. Run with and without it
// and the hashes must MATCH: that is the identity property, checked rather
// than asserted.
//
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "output/sha256.h"
#include "output/wav_writer.h"
#include "sfx/bank.h"
#include "sfx/pcm_cache.h"
#include "sfx/script.h"
#include "sfx/sfx_engine.h"
#include "synth/tables.h"

using namespace dsnd;

namespace {

bool read_file(const char* path, std::vector<u8>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

void report(const LoadResult& r, const char* what, bool quiet) {
    if (!quiet)
        for (const auto& w : r.warnings) std::fprintf(stderr, "%s: warning: %s\n", what, w.c_str());
    for (const auto& e : r.errors) std::fprintf(stderr, "%s: error: %s\n", what, e.c_str());
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    // `dsound_render <bank> --variant-hashes` is a second mode with no script
    // and no output file: it renders every variant in isolation through
    // render_variant -- the cache's own renderer -- and prints a hash of the
    // raw Q8.24 bytes.
    //
    // It exists so test/wasm_variant.mjs can compare the editor's
    // WebSfx::renderVariant against the native function sample for sample.
    // Those bytes are what the Bank tab draws as a waveform and what its peak
    // column measures, so if they ever diverge from what the engine caches,
    // the tool is showing something the game does not play.
    const bool variant_mode = argc >= 3 && std::string(argv[2]) == "--variant-hashes";

    if (argc < 4 && !variant_mode) {
        std::fprintf(stderr,
                     "usage: dsound_render <bank.sfxb|.sfxproj> <script.sfxscript> <out.wav>\n"
                     "       [--hash] [--no-cache] [--budget-kb N] [--seconds N] [--quiet]\n"
                     "       dsound_render <bank.sfxb|.sfxproj> --variant-hashes\n");
        return 2;
    }
    const char* bank_path = argv[1];
    const char* script_path = variant_mode ? "" : argv[2];
    const char* out_path = variant_mode ? "" : argv[3];

    bool want_hash = false, no_cache = false, quiet = false;
    u32 budget_kb = DEFAULT_CACHE_BUDGET_KB;
    i32 seconds_override = 0;
    for (int i = variant_mode ? 3 : 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--hash") want_hash = true;
        else if (a == "--no-cache") no_cache = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--budget-kb" && i + 1 < argc) budget_kb = static_cast<u32>(atoi(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) seconds_override = atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
            return 2;
        }
    }

    msynth::tables_init();

    std::vector<u8> bank_bytes;
    if (!read_file(bank_path, bank_bytes)) {
        std::fprintf(stderr, "cannot read %s\n", bank_path);
        return 1;
    }

    WaveTableCache wtc;
    SoundBank bank;
    LoadResult lr;
    if (ends_with(bank_path, ".sfxproj")) {
        lr = load_bank_json(reinterpret_cast<const char*>(bank_bytes.data()), bank_bytes.size(),
                            wtc, bank);
    } else {
        lr = load_bank(bank_bytes.data(), bank_bytes.size(), wtc, bank);
    }
    report(lr, "bank", quiet);
    if (!lr.ok) return 1;

    if (variant_mode) {
        // One line per variant: index, variant, sha256 of the raw Q8.24
        // bytes, frame count, whether it leaks a live voice, and the name.
        SfxVoicePool pool;
        const u32 max_blocks =
            (MAX_CACHE_MS * SAMPLE_RATE / 1000u + CONTROL_BLOCK - 1) / CONTROL_BLOCK;
        for (size_t i = 0; i < bank.sounds.size(); ++i) {
            const SoundDef& d = bank.sounds[i];
            for (u16 v = 0; v < d.variant_count; ++v) {
                std::vector<i32> pcm;
                u32 got = 0;
                bool leaks = false;
                const bool ok = render_variant(bank, i, v, /*mono=*/false, max_blocks, pcm, &got,
                                               &leaks, &pool);
                const std::string hex =
                    pcm.empty() ? std::string(64, '0')
                                : msynth::sha256_hex(pcm.data(), pcm.size() * sizeof(i32));
                std::printf("%zu %u %s %u %d %d %s\n", i, v, hex.c_str(), got,
                            ok ? 1 : 0, leaks ? 1 : 0, d.name.c_str());
            }
        }
        return 0;
    }

    // --no-cache is the other half of the identity check: same script, same
    // hash, different code path.
    if (no_cache)
        for (auto& s : bank.sounds) s.live = true;

    PcmCache cache;
    CacheReport crep;
    LoadResult cr = cache.build(bank, budget_kb * 1024, &crep);
    report(cr, "cache", quiet);
    if (!cr.ok) return 1;

    std::vector<u8> script_bytes;
    if (!read_file(script_path, script_bytes)) {
        std::fprintf(stderr, "cannot read %s\n", script_path);
        return 1;
    }
    Script script;
    LoadResult sr = load_script(reinterpret_cast<const char*>(script_bytes.data()),
                                script_bytes.size(), script);
    report(sr, "script", quiet);
    if (!sr.ok) return 1;
    if (seconds_override > 0) script.seconds = static_cast<u32>(seconds_override);

    SfxEngine engine;
    engine.set_content(&bank, &cache);

    // Queue everything up front with absolute `when` values. The engine's own
    // "apply at or before now" rule then places each trigger on the control
    // block a gameplay trigger would have landed on.
    for (const auto& k : script.listener) {
        SfxCommand c;
        c.when = k.at_sample;
        c.type = SfxCommand::Type::SetListener;
        c.a = k.x_q16;
        c.b = k.y_q16;
        c.c = k.z_q16;
        c.e = k.yaw_brad;
        engine.queue().push(c);
    }
    int unknown = 0;
    for (const auto& e : script.events) {
        const int id = bank.sound_index(e.sound);
        if (id < 0) {
            std::fprintf(stderr, "script: unknown sound '%s'\n", e.sound.c_str());
            ++unknown;
            continue;
        }
        SfxCommand c;
        c.when = e.at_sample;
        c.type = SfxCommand::Type::Play;
        c.handle = engine.next_handle();
        c.a = id;
        c.b = e.has_pos ? e.x_q16 : e.pan_q16;
        c.c = e.y_q16;
        c.d = e.z_q16;
        c.e = e.seed;
        engine.queue().push(c);
    }
    if (unknown) return 1;

    const size_t frames = static_cast<size_t>(script.seconds) * SAMPLE_RATE;
    const size_t whole = (frames / CONTROL_BLOCK) * CONTROL_BLOCK;
    std::vector<i16> pcm(whole * 2, 0);

    // In buffer-sized chunks, the way a real callback would, so the command
    // queue is drained on the same schedule it will be in a game.
    const size_t chunk = 512;
    for (size_t at = 0; at < whole; at += chunk) {
        const size_t n = at + chunk <= whole ? chunk : whole - at;
        engine.render(pcm.data() + at * 2, static_cast<int>(n));
    }

    if (!msynth::write_wav16(out_path, pcm, SAMPLE_RATE, 2)) {
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }

    if (!quiet) {
        const Diagnostics& d = engine.diag();
        std::fprintf(stderr,
                     "%s: %zu frames, %u cached / %u live variants, %u KB\n"
                     "  played %lld  stolen %lld  rejected: cooldown %lld, maxInstances %lld, "
                     "priority %lld, voices %lld\n"
                     "  peak instances %d, peak synth voices %d\n",
                     out_path, whole, crep.cached_variants, crep.live_variants, crep.bytes / 1024,
                     (long long)d.played, (long long)d.stolen, (long long)d.rejected_cooldown,
                     (long long)d.rejected_max_instances, (long long)d.rejected_priority,
                     (long long)d.rejected_no_voices, d.peak_instances, d.peak_synth_voices);
    }
    if (want_hash)
        std::printf("sha256 %s\n",
                    msynth::sha256_hex(pcm.data(), pcm.size() * sizeof(i16)).c_str());
    return 0;
}
