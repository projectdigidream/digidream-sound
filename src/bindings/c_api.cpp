#include "c_api.h"

#include <cstring>
#include <memory>
#include <string>

#include "sfx/bank.h"
#include "sfx/ms.h"
#include "sfx/pcm_cache.h"
#include "sfx/sfx_engine.h"
#include "synth/tables.h"

using namespace dsnd;

struct dsnd_ctx {
    WaveTableCache wtc;
    SoundBank bank;
    PcmCache cache;
    CacheReport cache_report;
    SfxEngine engine;
    bool loaded = false;
};

namespace {

void write_report(const LoadResult& r, char* out, size_t cap) {
    if (!out || cap == 0) return;
    std::string s;
    for (const auto& e : r.errors) s += "error: " + e + "\n";
    for (const auto& w : r.warnings) s += "warning: " + w + "\n";
    if (s.size() >= cap) s.resize(cap - 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
}

// Queue a command. A full queue drops it and counts the drop rather than
// blocking: the audio thread must never wait on the control thread, and a
// host that overruns 1024 pending commands has a problem this cannot fix.
void post(dsnd_ctx* ctx, const SfxCommand& c) {
    if (ctx) ctx->engine.queue().push(c);
}

}  // namespace

dsnd_ctx* dsnd_create(void) {
    // Idempotent, and it must happen before any patch is built or any voice
    // runs. Doing it here means a host cannot forget.
    msynth::tables_init();
    return new dsnd_ctx();
}

void dsnd_destroy(dsnd_ctx* ctx) { delete ctx; }

int dsnd_load_bank(dsnd_ctx* ctx, const uint8_t* data, size_t len, char* report, size_t cap) {
    if (!ctx) return 0;
    dsnd_unload_bank(ctx);
    LoadResult r = load_bank(data, len, ctx->wtc, ctx->bank);
    write_report(r, report, cap);
    if (!r.ok) return 0;
    ctx->engine.set_content(&ctx->bank, &ctx->cache);
    ctx->loaded = true;
    return 1;
}

int dsnd_load_bank_json(dsnd_ctx* ctx, const char* json, size_t len, char* report, size_t cap) {
    if (!ctx) return 0;
    dsnd_unload_bank(ctx);
    LoadResult r = load_bank_json(json, len, ctx->wtc, ctx->bank);
    write_report(r, report, cap);
    if (!r.ok) return 0;
    ctx->engine.set_content(&ctx->bank, &ctx->cache);
    ctx->loaded = true;
    return 1;
}

int dsnd_build_cache(dsnd_ctx* ctx, uint32_t budget_kb, char* report, size_t cap) {
    if (!ctx || !ctx->loaded) return 0;
    const u32 budget = (budget_kb ? budget_kb : DEFAULT_CACHE_BUDGET_KB) * 1024u;
    LoadResult r = ctx->cache.build(ctx->bank, budget, &ctx->cache_report);
    write_report(r, report, cap);
    if (!r.ok) return 0;
    // The cache changed which variants are cached, and the engine holds the
    // pointers, so it has to be told again.
    ctx->engine.set_content(&ctx->bank, &ctx->cache);
    return 1;
}

void dsnd_unload_bank(dsnd_ctx* ctx) {
    if (!ctx) return;
    // Order matters: the engine holds pointers into the bank's patches and the
    // cache's PCM, so it must let go first.
    ctx->engine.set_content(nullptr, nullptr);
    ctx->cache.clear();
    ctx->bank = SoundBank{};
    ctx->wtc = WaveTableCache{};
    ctx->cache_report = CacheReport{};
    ctx->loaded = false;
}

int dsnd_sound_id(const dsnd_ctx* ctx, const char* name) {
    if (!ctx || !name) return -1;
    return ctx->bank.sound_index(name);
}

int dsnd_bus_id(const dsnd_ctx* ctx, const char* name) {
    if (!ctx || !name) return -1;
    return ctx->bank.bus_index(name);
}

int dsnd_sound_count(const dsnd_ctx* ctx) {
    return ctx ? static_cast<int>(ctx->bank.sounds.size()) : 0;
}

int dsnd_bus_count(const dsnd_ctx* ctx) {
    return ctx ? static_cast<int>(ctx->bank.buses.size()) : 0;
}

const char* dsnd_sound_name(const dsnd_ctx* ctx, int sound_id) {
    if (!ctx || sound_id < 0 || static_cast<size_t>(sound_id) >= ctx->bank.sounds.size())
        return nullptr;
    return ctx->bank.sounds[static_cast<size_t>(sound_id)].name.c_str();
}

void dsnd_set_listener(dsnd_ctx* ctx, int32_t x_q16, int32_t y_q16, int32_t z_q16,
                       uint32_t yaw_brad) {
    SfxCommand c;
    c.type = SfxCommand::Type::SetListener;
    c.a = x_q16;
    c.b = y_q16;
    c.c = z_q16;
    c.e = yaw_brad;
    post(ctx, c);
}

uint32_t dsnd_play(dsnd_ctx* ctx, int sound_id, int variant, uint32_t seed, int32_t x_q16,
                   int32_t y_q16, int32_t z_q16) {
    if (!ctx || !ctx->loaded) return 0;
    SfxCommand c;
    c.type = SfxCommand::Type::Play;
    c.handle = ctx->engine.next_handle();
    c.a = sound_id;
    c.b = x_q16;
    c.c = y_q16;
    c.d = z_q16;
    c.e = seed;
    c.variant = variant;
    post(ctx, c);
    return c.handle;
}

uint32_t dsnd_play_2d(dsnd_ctx* ctx, int sound_id, int variant, uint32_t seed, int32_t pan_q16) {
    if (!ctx || !ctx->loaded) return 0;
    SfxCommand c;
    c.type = SfxCommand::Type::Play;
    c.handle = ctx->engine.next_handle();
    c.a = sound_id;
    c.b = pan_q16;
    c.c = 0;
    c.d = 0;
    c.e = seed;
    c.variant = variant;
    post(ctx, c);
    return c.handle;
}

void dsnd_stop(dsnd_ctx* ctx, uint32_t handle, uint32_t fade_ms) {
    SfxCommand c;
    c.type = SfxCommand::Type::Stop;
    c.handle = handle;
    c.a = static_cast<i32>(ms_to_blocks(static_cast<i32>(fade_ms)));
    post(ctx, c);
}

void dsnd_stop_bus(dsnd_ctx* ctx, int bus_id, uint32_t fade_ms) {
    SfxCommand c;
    c.type = SfxCommand::Type::StopBus;
    c.a = bus_id;
    c.b = static_cast<i32>(ms_to_blocks(static_cast<i32>(fade_ms)));
    post(ctx, c);
}

void dsnd_stop_all(dsnd_ctx* ctx, uint32_t fade_ms) {
    SfxCommand c;
    c.type = SfxCommand::Type::StopAll;
    c.a = static_cast<i32>(ms_to_blocks(static_cast<i32>(fade_ms)));
    post(ctx, c);
}

int dsnd_alive(const dsnd_ctx* ctx, uint32_t handle) {
    return ctx && ctx->engine.alive(handle) ? 1 : 0;
}

void dsnd_set_position(dsnd_ctx* ctx, uint32_t handle, int32_t x_q16, int32_t y_q16,
                       int32_t z_q16) {
    SfxCommand c;
    c.type = SfxCommand::Type::SetPosition;
    c.handle = handle;
    c.a = x_q16;
    c.b = y_q16;
    c.c = z_q16;
    post(ctx, c);
}

void dsnd_set_gain(dsnd_ctx* ctx, uint32_t handle, int32_t gain_q16) {
    SfxCommand c;
    c.type = SfxCommand::Type::SetGain;
    c.handle = handle;
    c.a = gain_q16;
    post(ctx, c);
}

void dsnd_set_pitch(dsnd_ctx* ctx, uint32_t handle, int32_t cents_q16) {
    SfxCommand c;
    c.type = SfxCommand::Type::SetPitch;
    c.handle = handle;
    c.a = cents_q16;
    post(ctx, c);
}

void dsnd_bus_gain(dsnd_ctx* ctx, int bus_id, int32_t gain_q16, uint32_t ramp_ms) {
    SfxCommand c;
    c.type = SfxCommand::Type::BusGain;
    c.a = bus_id;
    c.b = gain_q16;
    c.c = static_cast<i32>(ms_to_blocks(static_cast<i32>(ramp_ms)));
    post(ctx, c);
}

void dsnd_master_volume_milli(dsnd_ctx* ctx, int milli) {
    SfxCommand c;
    c.type = SfxCommand::Type::MasterVolume;
    c.a = static_cast<i32>((static_cast<i64>(milli) * GAIN_ONE) / 1000);
    post(ctx, c);
}

void dsnd_master_param(dsnd_ctx* ctx, int id, int32_t value) {
    SfxCommand c;
    c.type = SfxCommand::Type::MasterParam;
    c.a = id;
    c.b = value;
    post(ctx, c);
}

void dsnd_set_dither(dsnd_ctx* ctx, int enabled) {
    SfxCommand c;
    c.type = SfxCommand::Type::Dither;
    c.a = enabled;
    post(ctx, c);
}

int dsnd_music_duck_milli(const dsnd_ctx* ctx) {
    return ctx ? ctx->engine.music_duck_milli() : 1000;
}

void dsnd_render(dsnd_ctx* ctx, int16_t* out_interleaved, int frames) {
    if (!ctx || !out_interleaved || frames <= 0) return;
    ctx->engine.render(out_interleaved, frames);
}

void dsnd_diag_get(const dsnd_ctx* ctx, dsnd_diag* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!ctx) return;
    const Diagnostics& d = ctx->engine.diag();
    out->instances = d.instances;
    out->retiring = d.retiring;
    out->peak_instances = d.peak_instances;
    out->live_instances = d.live_instances;
    out->cached_instances = d.cached_instances;
    out->synth_voices = d.synth_voices;
    out->peak_synth_voices = d.peak_synth_voices;
    out->played = d.played;
    out->stolen = d.stolen;
    out->rejected_cooldown = d.rejected_cooldown;
    out->rejected_max_instances = d.rejected_max_instances;
    out->rejected_priority = d.rejected_priority;
    out->rejected_bus_budget = d.rejected_bus_budget;
    out->rejected_no_voices = d.rejected_no_voices;
    out->rejected_unknown = d.rejected_unknown;
    out->queue_dropped = d.queue_dropped;
    out->queue_high_water = d.queue_high_water;
    out->cache_bytes = ctx->cache_report.bytes;
    out->cached_variants = ctx->cache_report.cached_variants;
    out->live_variants = ctx->cache_report.live_variants;
}

uint64_t dsnd_now(const dsnd_ctx* ctx) { return ctx ? ctx->engine.now() : 0; }
