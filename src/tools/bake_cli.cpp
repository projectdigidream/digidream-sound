//
// dsound_bake -- .sfxproj -> .sfxb, and the seal that proves a bank renders
// the same everywhere.
//
//   dsound_bake <in.sfxproj> <out.sfxb> [--seal <script.sfxscript>] [--quiet]
//   dsound_bake --check <bank.sfxb> <script.sfxscript>
//   dsound_bake --info <bank.sfxb>
//
// --seal renders the given script and writes its sha256 into the bank's META.
// --check re-renders and compares: if a bank plays differently on this machine
// than on the one that baked it, this is what says so, and it says so before a
// player hears it rather than after.
//
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "output/sha256.h"
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

bool write_file(const char* path, const std::vector<u8>& data) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t n = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return n == data.size();
}

void report(const LoadResult& r, const char* what, bool quiet) {
    if (!quiet)
        for (const auto& w : r.warnings) std::fprintf(stderr, "%s: warning: %s\n", what, w.c_str());
    for (const auto& e : r.errors) std::fprintf(stderr, "%s: error: %s\n", what, e.c_str());
}

// Render a script through a bank and hash the result. Shared by --seal and
// --check so the two cannot disagree about what they are measuring.
bool render_hash(const std::vector<u8>& bank_bytes, const char* script_path, u8 out[32],
                 bool quiet) {
    WaveTableCache wtc;
    SoundBank bank;
    LoadResult lr = load_bank(bank_bytes.data(), bank_bytes.size(), wtc, bank);
    report(lr, "bank", quiet);
    if (!lr.ok) return false;

    PcmCache cache;
    LoadResult cr = cache.build(bank, DEFAULT_CACHE_BUDGET_KB * 1024);
    report(cr, "cache", quiet);
    if (!cr.ok) return false;

    std::vector<u8> sb;
    if (!read_file(script_path, sb)) {
        std::fprintf(stderr, "cannot read %s\n", script_path);
        return false;
    }
    Script script;
    LoadResult sr = load_script(reinterpret_cast<const char*>(sb.data()), sb.size(), script);
    report(sr, "script", quiet);
    if (!sr.ok) return false;

    SfxEngine engine;
    engine.set_content(&bank, &cache);
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
    for (const auto& e : script.events) {
        const int id = bank.sound_index(e.sound);
        if (id < 0) {
            std::fprintf(stderr, "script: unknown sound '%s'\n", e.sound.c_str());
            return false;
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

    const size_t frames = static_cast<size_t>(script.seconds) * SAMPLE_RATE;
    const size_t whole = (frames / CONTROL_BLOCK) * CONTROL_BLOCK;
    std::vector<i16> pcm(whole * 2, 0);
    for (size_t at = 0; at < whole; at += 512) {
        const size_t n = at + 512 <= whole ? 512 : whole - at;
        engine.render(pcm.data() + at * 2, static_cast<int>(n));
    }

    msynth::Sha256 h;
    h.update(pcm.data(), pcm.size() * sizeof(i16));
    const std::string hex = h.hex_digest();
    auto nib = [](char c) -> u8 {
        if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
        return 0;
    };
    for (int i = 0; i < 32; ++i)
        out[i] = static_cast<u8>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    return true;
}

std::string to_hex(const u8 h[32]) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) {
        s += d[h[i] >> 4];
        s += d[h[i] & 15];
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: dsound_bake <in.sfxproj> <out.sfxb> [--seal <script>] [--quiet]\n"
                     "       dsound_bake --check <bank.sfxb> <script.sfxscript>\n"
                     "       dsound_bake --info  <bank.sfxb>\n");
        return 2;
    }
    msynth::tables_init();
    const std::string mode = argv[1];

    if (mode == "--info") {
        if (argc < 3) return 2;
        std::vector<u8> bytes;
        if (!read_file(argv[2], bytes)) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        BankMeta m;
        LoadResult r = read_bank_meta(bytes.data(), bytes.size(), m);
        report(r, "bank", false);
        if (!r.ok) return 1;
        std::printf("name          %s\n", m.name.c_str());
        std::printf("sounds        %u\n", m.sound_count);
        std::printf("variants      %u\n", m.variant_count);
        std::printf("patches       %u\n", m.patch_count);
        std::printf("content hash  %s\n", to_hex(m.content_hash).c_str());
        if (m.has_render_hash) {
            std::printf("render hash   %s\n", to_hex(m.render_hash).c_str());
            std::printf("render script %s\n", m.render_script.c_str());
        } else {
            std::printf("render hash   (unsealed)\n");
        }
        return 0;
    }

    if (mode == "--check") {
        if (argc < 4) return 2;
        std::vector<u8> bytes;
        if (!read_file(argv[2], bytes)) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        BankMeta m;
        if (!read_bank_meta(bytes.data(), bytes.size(), m).ok) return 1;
        if (!m.has_render_hash) {
            std::fprintf(stderr, "%s carries no render seal; bake it with --seal first\n", argv[2]);
            return 1;
        }
        u8 got[32];
        if (!render_hash(bytes, argv[3], got, false)) return 1;
        if (std::memcmp(got, m.render_hash, 32) != 0) {
            std::fprintf(stderr,
                         "RENDER MISMATCH\n  sealed %s\n  here   %s\n"
                         "This bank does not sound the same on this machine as on the one that\n"
                         "baked it. Something in the synthesis path has changed.\n",
                         to_hex(m.render_hash).c_str(), to_hex(got).c_str());
            return 1;
        }
        std::printf("ok %s\n", to_hex(got).c_str());
        return 0;
    }

    if (argc < 3) return 2;
    const char* in = argv[1];
    const char* out = argv[2];
    const char* seal_script = nullptr;
    bool quiet = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seal" && i + 1 < argc) seal_script = argv[++i];
        else if (a == "--quiet") quiet = true;
        else {
            std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
            return 2;
        }
    }

    std::vector<u8> src;
    if (!read_file(in, src)) {
        std::fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    std::vector<u8> baked;
    LoadResult r = bake_bank(reinterpret_cast<const char*>(src.data()), src.size(), baked);
    report(r, "bake", quiet);
    if (!r.ok) return 1;

    if (seal_script) {
        u8 h[32];
        if (!render_hash(baked, seal_script, h, quiet)) return 1;
        if (!seal_bank(baked, h, seal_script)) {
            std::fprintf(stderr, "could not write the render seal\n");
            return 1;
        }
        if (!quiet) std::fprintf(stderr, "sealed %s\n", to_hex(h).c_str());
    }

    if (!write_file(out, baked)) {
        std::fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    if (!quiet) std::fprintf(stderr, "%s: %zu bytes\n", out, baked.size());
    return 0;
}
