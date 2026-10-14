//
// The JavaScript face of the engine: one `WebSfx` class, designed to live
// inside an AudioWorklet.
//
// Everything runs in the worklet -- the wasm module is imported into worklet
// scope and the main thread drives it over the MessagePort. That is midisynth's
// arrangement and it is deliberate: the alternative (-sAUDIO_WORKLET=1) needs
// SharedArrayBuffer, which needs COOP/COEP headers, which is exactly the
// hosting dependency the whole tool chain is built to avoid.
//
// Binary in and out goes through the heap rather than through embind's value
// marshalling: `bankBufferPrepare(n)` hands JS a pointer to write into, and
// `render(frames)` hands back a pointer to read from. Copying a megabyte of
// bank through a JS array per call would be the most expensive thing here.
//
#include <emscripten/bind.h>
#include <unistd.h>   // sbrk, for heapUsedBytes

#include <cmath>
#include <string>
#include <vector>

#include "sfx/bank.h"
#include "sfx/pcm_cache.h"
#include "sfx/script.h"
#include "sfx/sfx_engine.h"
#include "sfx/sfx_json.h"
#include "synth/tables.h"

using namespace dsnd;

namespace {

void json_escape(std::string& s, const std::string& text) {
    for (char ch : text) {
        if (ch == '"' || ch == '\\') { s += '\\'; s += ch; continue; }
        if (ch == '\n') { s += "\\n"; continue; }
        if (static_cast<unsigned char>(ch) < 0x20) continue;  // never valid raw
        s += ch;
    }
}

// Split one diagnostic into the pieces an editor can act on.
//
// Every warn()/error() site in sfx_json.cpp builds its message as
// `where + ": " + prose`, and `where` is a dotted path assembled from the
// sound's name -- "sound 'weapon.pistol.fire'.attenuation.minDist", "sound 'x'
// variant 2", "bank", "fx", "master". So the structure is already there; it
// has just been flattened into prose by the time JS sees it.
//
// Splitting it HERE rather than in the loader is deliberate. LoadResult is
// midisynth's type (synth/patch_loader.h), shared with patch_loader.cpp which
// this project compiles from source rather than forking. Widening it to carry
// a structured form would fork that shared contract -- exactly the drift
// dsp_manifest.txt exists to prevent -- and would touch 48 call sites and the
// CLI's stderr format for no engine benefit.
//
// A message that does not fit the shape degrades to `path: ""` and the whole
// text, which the UI shows in its problems drawer rather than against a
// control. That is the honest failure: unattached, never swallowed.
struct Split {
    std::string path;   // "sound 'x'.attenuation.minDist", or ""
    std::string text;   // the prose after ": "
    int sound = -1;     // index into bank.sounds, or -1
    int variant = -1;   // index into that sound's variants, or -1
    std::string field;  // "attenuation.minDist", or ""
};

Split split_message(const std::string& msg, const SoundBank& bank) {
    Split out;
    const size_t colon = msg.find(": ");
    if (colon == std::string::npos) {
        out.text = msg;
        return out;
    }
    out.path = msg.substr(0, colon);
    out.text = msg.substr(colon + 2);

    // sound '<name>'[ variant <i>][.<field>]
    if (out.path.rfind("sound '", 0) != 0) return out;
    const size_t close = out.path.find('\'', 7);
    if (close == std::string::npos) return out;

    const std::string name = out.path.substr(7, close - 7);
    out.sound = bank.sound_index(name);

    std::string rest = out.path.substr(close + 1);
    const std::string vtag = " variant ";
    if (rest.rfind(vtag, 0) == 0) {
        size_t i = vtag.size();
        int v = 0;
        bool any = false;
        while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
            v = v * 10 + (rest[i] - '0');
            ++i;
            any = true;
        }
        if (any) out.variant = v;
        rest = rest.substr(i);
    }
    if (!rest.empty() && rest[0] == '.') out.field = rest.substr(1);
    return out;
}

void append_messages(std::string& s, const std::vector<std::string>& msgs,
                     const SoundBank& bank) {
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (i) s += ",";
        const Split p = split_message(msgs[i], bank);
        s += "{\"path\":\"";
        json_escape(s, p.path);
        s += "\",\"text\":\"";
        json_escape(s, p.text);
        s += "\",\"sound\":" + std::to_string(p.sound);
        s += ",\"variant\":" + std::to_string(p.variant);
        s += ",\"field\":\"";
        json_escape(s, p.field);
        s += "\"}";
    }
}

std::string report_json(const LoadResult& r, const SoundBank& bank) {
    std::string s = "{\"ok\":";
    s += r.ok ? "true" : "false";
    s += ",\"warnings\":[";
    append_messages(s, r.warnings, bank);
    s += "],\"errors\":[";
    append_messages(s, r.errors, bank);
    s += "]}";
    return s;
}

}  // namespace

class WebSfx {
  public:
    WebSfx() { msynth::tables_init(); }

    // ---- content ----

    uintptr_t bankBufferPrepare(int size) {
        buf_.assign(static_cast<size_t>(size > 0 ? size : 0), 0);
        return reinterpret_cast<uintptr_t>(buf_.data());
    }

    std::string loadBankFromBuffer() {
        release();
        LoadResult r = load_bank(buf_.data(), buf_.size(), wtc_, bank_);
        if (r.ok) {
            engine_.set_content(&bank_, &cache_);
            loaded_ = true;
        }
        return report_json(r, bank_);
    }

    std::string loadBankJson(const std::string& json) {
        release();
        LoadResult r = load_bank_json(json.c_str(), json.size(), wtc_, bank_);
        if (r.ok) {
            engine_.set_content(&bank_, &cache_);
            loaded_ = true;
        }
        return report_json(r, bank_);
    }

    // Costly. Uncached sounds play live meanwhile and are bit-identical to
    // their cached form, so a host may call this late, or never.
    std::string buildCache(int budget_kb) {
        if (!loaded_) return "{\"ok\":false,\"warnings\":[],\"errors\":[\"no bank\"]}";
        const u32 b = static_cast<u32>(budget_kb > 0 ? budget_kb : DEFAULT_CACHE_BUDGET_KB);
        LoadResult r = cache_.build(bank_, b * 1024u, &crep_);
        if (r.ok) engine_.set_content(&bank_, &cache_);
        return report_json(r, bank_);
    }

    void unloadBank() { release(); }

    int soundId(const std::string& name) const { return bank_.sound_index(name); }
    int busId(const std::string& name) const { return bank_.bus_index(name); }
    int soundCount() const { return static_cast<int>(bank_.sounds.size()); }
    std::string soundName(int i) const {
        if (i < 0 || static_cast<size_t>(i) >= bank_.sounds.size()) return "";
        return bank_.sounds[static_cast<size_t>(i)].name;
    }

    // The whole bank as JSON, for a tool that wants to list what it loaded
    // without parsing the binary itself.
    std::string bankInfoJson() const {
        std::string s = "{\"name\":\"" + bank_.name + "\",\"sounds\":[";
        for (size_t i = 0; i < bank_.sounds.size(); ++i) {
            const SoundDef& d = bank_.sounds[i];
            if (i) s += ",";
            s += "{\"name\":\"" + d.name + "\",\"bus\":" + std::to_string(d.bus) +
                 ",\"priority\":" + std::to_string(d.priority) +
                 ",\"maxInstances\":" + std::to_string(d.max_instances) +
                 ",\"variants\":" + std::to_string(d.variant_count) +
                 ",\"live\":" + (d.live ? "true" : "false") + "}";
        }
        s += "],\"buses\":[";
        for (size_t i = 0; i < bank_.buses.size(); ++i) {
            if (i) s += ",";
            s += "{\"name\":\"" + bank_.buses[i].name +
                 "\",\"maxInstances\":" + std::to_string(bank_.buses[i].max_instances) +
                 ",\"spatial\":" + (bank_.buses[i].spatial ? "true" : "false") + "}";
        }
        s += "]}";
        return s;
    }

    // ---- transport ----

    void setListener(int x_q16, int y_q16, int z_q16, unsigned yaw_brad) {
        SfxCommand c;
        c.type = SfxCommand::Type::SetListener;
        c.a = x_q16;
        c.b = y_q16;
        c.c = z_q16;
        c.e = yaw_brad;
        engine_.queue().push(c);
    }

    unsigned play(int sound, int variant, unsigned seed, int x, int y, int z) {
        if (!loaded_) return 0;
        SfxCommand c;
        c.type = SfxCommand::Type::Play;
        c.handle = engine_.next_handle();
        c.a = sound;
        c.b = x;
        c.c = y;
        c.d = z;
        c.e = seed;
        c.variant = variant;
        engine_.queue().push(c);
        return c.handle;
    }

    unsigned play2d(int sound, int variant, unsigned seed, int pan_q16) {
        return play(sound, variant, seed, pan_q16, 0, 0);
    }

    void stop(unsigned handle, int fade_blocks) {
        SfxCommand c;
        c.type = SfxCommand::Type::Stop;
        c.handle = handle;
        c.a = fade_blocks;
        engine_.queue().push(c);
    }

    // Parity with dsnd_stop_bus. The command existed and the JS face did not
    // expose it, which is a capability gap that looks exactly like a missing
    // feature from the outside; test/embind_parity.mjs now stops that
    // recurring by diffing this class against c_api.h.
    void stopBus(int bus, int fade_blocks) {
        SfxCommand c;
        c.type = SfxCommand::Type::StopBus;
        c.a = bus;
        c.b = fade_blocks;
        engine_.queue().push(c);
    }

    void stopAll(int fade_blocks) {
        SfxCommand c;
        c.type = SfxCommand::Type::StopAll;
        c.a = fade_blocks;
        engine_.queue().push(c);
    }

    // Parity with dsnd_set_pitch. Clamped by the engine to the sampler's rate
    // bounds -- one octave up, two down.
    void setPitch(unsigned handle, int cents_q16) {
        SfxCommand c;
        c.type = SfxCommand::Type::SetPitch;
        c.handle = handle;
        c.a = cents_q16;
        engine_.queue().push(c);
    }

    void setPosition(unsigned handle, int x, int y, int z) {
        SfxCommand c;
        c.type = SfxCommand::Type::SetPosition;
        c.handle = handle;
        c.a = x;
        c.b = y;
        c.c = z;
        engine_.queue().push(c);
    }

    void setGain(unsigned handle, int gain_q16) {
        SfxCommand c;
        c.type = SfxCommand::Type::SetGain;
        c.handle = handle;
        c.a = gain_q16;
        engine_.queue().push(c);
    }

    void busGain(int bus, int gain_q16, int ramp_blocks) {
        SfxCommand c;
        c.type = SfxCommand::Type::BusGain;
        c.a = bus;
        c.b = gain_q16;
        c.c = ramp_blocks;
        engine_.queue().push(c);
    }

    void masterVolumeMilli(int milli) {
        SfxCommand c;
        c.type = SfxCommand::Type::MasterVolume;
        c.a = static_cast<i32>((static_cast<i64>(milli) * GAIN_ONE) / 1000);
        engine_.queue().push(c);
    }

    void masterParam(int id, int value) {
        SfxCommand c;
        c.type = SfxCommand::Type::MasterParam;
        c.a = id;
        c.b = value;
        engine_.queue().push(c);
    }

    void setDither(bool on) {
        SfxCommand c;
        c.type = SfxCommand::Type::Dither;
        c.a = on ? 1 : 0;
        engine_.queue().push(c);
    }

    bool alive(unsigned handle) const { return engine_.alive(handle); }
    int musicDuckMilli() const { return engine_.music_duck_milli(); }

    // ---- render ----

    // Returns a pointer to `frames * 2` interleaved int16. The worklet reads
    // it through HEAP16 and de-interleaves into its float output; the bytes it
    // reads are the same bytes the offline renderer hashes, which is what
    // makes the wasm leg of the determinism gate meaningful.
    uintptr_t render(int frames) {
        if (frames <= 0) return 0;
        pcm_.assign(static_cast<size_t>(frames) * 2, 0);
        engine_.render(pcm_.data(), frames);
        return reinterpret_cast<uintptr_t>(pcm_.data());
    }

    std::string diagJson() const {
        const Diagnostics& d = engine_.diag();
        std::string s = "{";
        s += "\"instances\":" + std::to_string(d.instances);
        s += ",\"retiring\":" + std::to_string(d.retiring);
        s += ",\"peakInstances\":" + std::to_string(d.peak_instances);
        s += ",\"liveInstances\":" + std::to_string(d.live_instances);
        s += ",\"cachedInstances\":" + std::to_string(d.cached_instances);
        s += ",\"synthVoices\":" + std::to_string(d.synth_voices);
        s += ",\"peakSynthVoices\":" + std::to_string(d.peak_synth_voices);
        s += ",\"played\":" + std::to_string(d.played);
        s += ",\"stolen\":" + std::to_string(d.stolen);
        s += ",\"rejectedCooldown\":" + std::to_string(d.rejected_cooldown);
        s += ",\"rejectedMaxInstances\":" + std::to_string(d.rejected_max_instances);
        s += ",\"rejectedPriority\":" + std::to_string(d.rejected_priority);
        s += ",\"rejectedBusBudget\":" + std::to_string(d.rejected_bus_budget);
        s += ",\"rejectedUnknown\":" + std::to_string(d.rejected_unknown);
        // README.md advertises six refusal reasons and this used to report
        // five: a documented promise the JS face did not keep, and the missing
        // one is the least guessable of the six.
        s += ",\"rejectedNoVoices\":" + std::to_string(d.rejected_no_voices);
        s += ",\"queueDropped\":" + std::to_string(d.queue_dropped);
        s += ",\"queueHighWater\":" + std::to_string(d.queue_high_water);
        s += ",\"cacheBytes\":" + std::to_string(crep_.bytes);
        // Without the budget the byte count is a number with nothing to be
        // large or small against.
        s += ",\"cacheBudgetBytes\":" + std::to_string(crep_.budget_bytes);
        s += ",\"cachedVariants\":" + std::to_string(crep_.cached_variants);
        s += ",\"liveVariants\":" + std::to_string(crep_.live_variants);
        s += "}";
        return s;
    }

    // ---- analysis, for the editor ----

    // Render ONE variant in isolation and hand back unclipped Q8.24 i32.
    //
    // Not the same thing as playing it and capturing render(): that returns
    // i16, so anything above 0 dBFS has already been clamped and a peak column
    // fed from it can only ever say "0.0 dB" -- never by how much you are
    // over, which is the one thing a peak column is for. It also goes through
    // instance gain, bus gain, the spatialiser and the master chain, any of
    // which could be off by a ULP from the unity you assumed, leaving you
    // debugging the thumbnail instead of the sound.
    //
    // These bytes ARE the cache entry, so a waveform drawn from them is the
    // waveform the game plays. test/wasm_variant.mjs pins that equality.
    //
    // MUST NOT run concurrently with render(). Use a separate WebSfx -- the
    // editor's worker instance -- rather than the worklet's.
    uintptr_t renderVariant(int sound, int variant, bool mono) {
        vinfo_ok_ = false;
        vframes_ = 0;
        vleaks_ = false;
        vstereo_ = !mono;
        vbuf_.clear();
        if (!loaded_) return 0;
        if (sound < 0 || static_cast<size_t>(sound) >= bank_.sounds.size()) return 0;
        // The variant bound is NOT optional. render_variant indexes straight
        // into the variant table, so an out-of-range index here is a wasm
        // memory-access trap -- which in a browser kills the worker with a
        // RuntimeError and no usable diagnostic. The engine's play path clamps
        // instead, but this is an analysis entry point reached directly from
        // JS, where the caller's index is whatever the UI last had.
        if (variant < 0 || variant >= bank_.sounds[static_cast<size_t>(sound)].variant_count)
            return 0;

        const u32 max_blocks = (MAX_CACHE_MS * SAMPLE_RATE / 1000u + CONTROL_BLOCK - 1)
                             / CONTROL_BLOCK;
        vinfo_ok_ = render_variant(bank_, static_cast<size_t>(sound),
                                   static_cast<u16>(variant), mono, max_blocks,
                                   vbuf_, &vframes_, &vleaks_, &vpool_);
        if (vbuf_.empty()) return 0;
        return reinterpret_cast<uintptr_t>(vbuf_.data());
    }

    // Peak is computed here, from the unclipped samples, so the UI needs no
    // float pipeline and a value above full scale reports as positive.
    std::string renderVariantInfoJson() const {
        i64 peak = 0;
        for (i32 s : vbuf_) {
            const i64 a = s < 0 ? -static_cast<i64>(s) : static_cast<i64>(s);
            if (a > peak) peak = a;
        }
        // 20*log10(peak / 2^24), in millibels, by integer bisection on the
        // same table-free arithmetic the engine uses elsewhere.
        int milli = -96000;
        if (peak > 0) {
            double ratio = static_cast<double>(peak) / static_cast<double>(1 << SAMPLE_SHIFT);
            milli = static_cast<int>(20000.0 * std::log10(ratio));
            if (milli < -96000) milli = -96000;
        }
        std::string s = "{\"ok\":";
        s += vinfo_ok_ ? "true" : "false";
        s += ",\"frames\":" + std::to_string(vframes_);
        s += ",\"stereo\":";
        s += vstereo_ ? "true" : "false";
        s += ",\"leaks\":";
        s += vleaks_ ? "true" : "false";
        s += ",\"peakQ824\":" + std::to_string(peak);
        s += ",\"peakDbMilli\":" + std::to_string(milli);
        s += ",\"bytes\":" + std::to_string(vbuf_.size() * sizeof(i32));
        s += "}";
        return s;
    }

    // ---- baking ----

    // .sfxproj -> .sfxb, in the browser.
    //
    // A standalone product whose users cannot produce the shipping artifact
    // without a native CLI has a hole where its output should be.
    //
    // The bytes are held here and read through bakedPtr/bakedSize rather than
    // returned: embind decodes a returned std::string as UTF-8, which mangles
    // every byte above 0x7F. midisynth's bakeTrackFromBuffer does the same
    // thing for the same reason.
    std::string bakeToBuffer(const std::string& json) {
        baked_.clear();
        SoundBank scratch;  // named only so diagnostics can resolve indices
        LoadResult r = bake_bank(json.c_str(), json.size(), baked_);
        if (!r.ok) baked_.clear();
        return report_json(r, scratch);
    }
    uintptr_t bakedPtr() const { return reinterpret_cast<uintptr_t>(baked_.data()); }
    int bakedSize() const { return static_cast<int>(baked_.size()); }

    // ---- instrumentation ----

    // The arena's high-water mark against a fixed INITIAL_MEMORY. The heap
    // cannot grow, so a host that wants to avoid an abort has to be able to
    // see the wall before it hits it -- and test/wasm_memory.mjs pins these
    // numbers the way goldens.txt pins hashes.
    int heapUsedBytes() const { return static_cast<int>(reinterpret_cast<uintptr_t>(sbrk(0))); }

    // ---- offline, for the determinism gate ----

    // Queue a whole .sfxscript at absolute sample positions, exactly as
    // dsound_render does natively. This is what makes the native-vs-wasm leg
    // a comparison of the same program rather than of two similar ones.
    std::string queueScript(const std::string& json) {
        Script script;
        LoadResult r = load_script(json.c_str(), json.size(), script);
        if (!r.ok) return report_json(r, bank_);
        for (const auto& k : script.listener) {
            SfxCommand c;
            c.when = k.at_sample;
            c.type = SfxCommand::Type::SetListener;
            c.a = k.x_q16;
            c.b = k.y_q16;
            c.c = k.z_q16;
            c.e = k.yaw_brad;
            engine_.queue().push(c);
        }
        for (const auto& e : script.events) {
            const int id = bank_.sound_index(e.sound);
            if (id < 0) {
                r.error("unknown sound '" + e.sound + "'");
                continue;
            }
            SfxCommand c;
            c.when = e.at_sample;
            c.type = SfxCommand::Type::Play;
            c.handle = engine_.next_handle();
            c.a = id;
            c.b = e.has_pos ? e.x_q16 : e.pan_q16;
            c.c = e.y_q16;
            c.d = e.z_q16;
            c.e = e.seed;
            c.variant = e.variant;
            engine_.queue().push(c);
        }
        return report_json(r, bank_);
    }

    int scriptSeconds(const std::string& json) const {
        Script script;
        if (!load_script(json.c_str(), json.size(), script).ok) return 0;
        return static_cast<int>(script.seconds);
    }

  private:
    void release() {
        engine_.set_content(nullptr, nullptr);
        cache_.clear();
        bank_ = SoundBank{};
        wtc_ = WaveTableCache{};
        crep_ = CacheReport{};
        loaded_ = false;
    }

    WaveTableCache wtc_;
    SoundBank bank_;
    PcmCache cache_;
    CacheReport crep_;
    SfxEngine engine_;
    std::vector<u8> buf_;
    std::vector<i16> pcm_;
    bool loaded_ = false;

    // renderVariant's state. The pool is a member because it is 48 voices of
    // ~17 KB -- 820 KB built and torn down per call -- and a thumbnail pass
    // over a 101-sound bank makes 101 calls.
    std::vector<i32> vbuf_;
    SfxVoicePool vpool_;
    u32 vframes_ = 0;
    bool vinfo_ok_ = false;
    bool vleaks_ = false;
    bool vstereo_ = true;

    std::vector<u8> baked_;
};

EMSCRIPTEN_BINDINGS(dsound) {
    emscripten::class_<WebSfx>("WebSfx")
        .constructor<>()
        .function("bankBufferPrepare", &WebSfx::bankBufferPrepare)
        .function("loadBankFromBuffer", &WebSfx::loadBankFromBuffer)
        .function("loadBankJson", &WebSfx::loadBankJson)
        .function("buildCache", &WebSfx::buildCache)
        .function("unloadBank", &WebSfx::unloadBank)
        .function("soundId", &WebSfx::soundId)
        .function("busId", &WebSfx::busId)
        .function("soundCount", &WebSfx::soundCount)
        .function("soundName", &WebSfx::soundName)
        .function("bankInfoJson", &WebSfx::bankInfoJson)
        .function("setListener", &WebSfx::setListener)
        .function("play", &WebSfx::play)
        .function("play2d", &WebSfx::play2d)
        .function("stop", &WebSfx::stop)
        .function("stopBus", &WebSfx::stopBus)
        .function("stopAll", &WebSfx::stopAll)
        .function("setPitch", &WebSfx::setPitch)
        .function("setPosition", &WebSfx::setPosition)
        .function("setGain", &WebSfx::setGain)
        .function("busGain", &WebSfx::busGain)
        .function("masterVolumeMilli", &WebSfx::masterVolumeMilli)
        .function("masterParam", &WebSfx::masterParam)
        .function("setDither", &WebSfx::setDither)
        .function("alive", &WebSfx::alive)
        .function("musicDuckMilli", &WebSfx::musicDuckMilli)
        .function("render", &WebSfx::render)
        .function("diagJson", &WebSfx::diagJson)
        .function("queueScript", &WebSfx::queueScript)
        .function("scriptSeconds", &WebSfx::scriptSeconds)
        .function("renderVariant", &WebSfx::renderVariant)
        .function("renderVariantInfoJson", &WebSfx::renderVariantInfoJson)
        .function("bakeToBuffer", &WebSfx::bakeToBuffer)
        .function("bakedPtr", &WebSfx::bakedPtr)
        .function("bakedSize", &WebSfx::bakedSize)
        .function("heapUsedBytes", &WebSfx::heapUsedBytes);
}
