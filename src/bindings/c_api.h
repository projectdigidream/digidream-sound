#ifndef DSND_C_API_H
#define DSND_C_API_H
//
// DigidreamSound -- the C embedding surface.
//
// This is the whole contract. A host that speaks this header needs to read
// nothing else in the project: everything below is either a call it makes or
// a rule it must not break.
//
// THREADING.
//   dsnd_render is the ONLY call that belongs on the audio thread.
//   Everything else is control-thread and reaches the engine through a
//   lock-free queue, so it may be called at any time -- EXCEPT
//   dsnd_load_bank*, dsnd_build_cache and dsnd_unload_bank, which rebuild the
//   content the audio thread is reading and must not run concurrently with
//   dsnd_render. Same rule, and same reason, as midisynth's C API.
//
// COORDINATES.
//   Simulation space: X and Y on the ground, Z up, Q16.16 world units, and
//   the listener's yaw as binary radians about +Z (0 = facing +X), per
//   core/include/dnp/math/trig.hpp. NOT the renderer's space. Take the SIM
//   yaw -- a Three.js camera is Y-up with Z = -y, and its yaw will pan
//   everything backwards. One world unit is 48 Doom units, about 1.46 m.
//
// SEEDS.
//   dsnd_play takes a seed and it is not optional. It drives variant choice,
//   pitch jitter, gain jitter and the synthesis noise, in a fixed order, so
//   two machines that agree on the simulation agree on the sound. Derive it
//   from something stable -- an event id, an entity id, a tick -- never from
//   a clock or a counter that differs between clients.
//
// A HANDLE IS NOT A PROMISE.
//   dsnd_play returns non-zero as soon as the trigger is queued, but whether
//   it is ever audible is decided on the audio thread: a cooldown, the
//   sound's instance cap, the bus budget or a higher-priority neighbour can
//   all refuse it. Use dsnd_alive, and read dsnd_diag for the reason -- every
//   refusal is counted separately, because "the sound did not play" is
//   otherwise the least debuggable complaint in a game.
//
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dsnd_ctx dsnd_ctx;

dsnd_ctx* dsnd_create(void);
void      dsnd_destroy(dsnd_ctx* ctx);

/* ---- Content. Control thread, NOT concurrent with dsnd_render. ---------- */

/* A baked .sfxb. Returns 1 on success; `report` receives warnings and errors
   as text, whether or not it succeeded. */
int dsnd_load_bank(dsnd_ctx* ctx, const uint8_t* data, size_t len, char* report, size_t cap);

/* A .sfxproj, for tools. Slower -- it builds every wavetable from its recipe. */
int dsnd_load_bank_json(dsnd_ctx* ctx, const char* json, size_t len, char* report, size_t cap);

/* Render the non-live sounds into the PCM cache.
 *
 * Costly, and deliberately separate from loading so a host can show progress.
 * Uncached sounds play live in the meantime and are BIT-IDENTICAL to their
 * cached form, so a half-built cache is inaudible rather than a seam.
 *
 * Returns 1 when the cache is complete, 0 when there is more to do. Call
 * again until it returns 1, or leave it: what is not cached simply stays
 * live. `budget_kb` of 0 uses the default (8 MB). */
int dsnd_build_cache(dsnd_ctx* ctx, uint32_t budget_kb, char* report, size_t cap);

void dsnd_unload_bank(dsnd_ctx* ctx);

/* Resolve names ONCE, at map load. The hot path takes ids. -1 = unknown. */
int  dsnd_sound_id(const dsnd_ctx* ctx, const char* name);
int  dsnd_bus_id(const dsnd_ctx* ctx, const char* name);
int  dsnd_sound_count(const dsnd_ctx* ctx);
int  dsnd_bus_count(const dsnd_ctx* ctx);
/* Tooling only -- valid until the bank is unloaded. */
const char* dsnd_sound_name(const dsnd_ctx* ctx, int sound_id);

/* ---- Listener (queued) -------------------------------------------------- */

void dsnd_set_listener(dsnd_ctx* ctx, int32_t x_q16, int32_t y_q16, int32_t z_q16,
                       uint32_t yaw_brad);

/* ---- Play and stop (queued) --------------------------------------------- */

/* Positional. `variant` of -1 picks by weight from `seed`.
 *
 * THE BUS DECIDES, not the call. A sound on a bus with "spatial": false
 * ignores the position and takes x_q16 as a direct pan -- so calling this for
 * a UI sound puts it hard left rather than in front of you. Use dsnd_play_2d
 * for those; it is the same call with the argument named honestly. */
uint32_t dsnd_play(dsnd_ctx* ctx, int sound_id, int variant, uint32_t seed,
                   int32_t x_q16, int32_t y_q16, int32_t z_q16);

/* Non-positional: UI, the player's own weapon, anything that is not in the
   world. `pan_q16` runs -65536 (hard left) to 65536 (hard right). */
uint32_t dsnd_play_2d(dsnd_ctx* ctx, int sound_id, int variant, uint32_t seed, int32_t pan_q16);

/* A fade of 0 cuts immediately. A stale handle is a silent no-op. */
void dsnd_stop(dsnd_ctx* ctx, uint32_t handle, uint32_t fade_ms);
void dsnd_stop_bus(dsnd_ctx* ctx, int bus_id, uint32_t fade_ms);
void dsnd_stop_all(dsnd_ctx* ctx, uint32_t fade_ms);
int  dsnd_alive(const dsnd_ctx* ctx, uint32_t handle);

void dsnd_set_position(dsnd_ctx* ctx, uint32_t handle, int32_t x_q16, int32_t y_q16, int32_t z_q16);
void dsnd_set_gain(dsnd_ctx* ctx, uint32_t handle, int32_t gain_q16);
/* Clamped to the sampler's rate bounds (one octave up, two down). */
void dsnd_set_pitch(dsnd_ctx* ctx, uint32_t handle, int32_t cents_q16);

/* ---- Mix (queued) ------------------------------------------------------- */

void dsnd_bus_gain(dsnd_ctx* ctx, int bus_id, int32_t gain_q16, uint32_t ramp_ms);
void dsnd_master_volume_milli(dsnd_ctx* ctx, int milli);
/* `id` indexes midisynth's MasterParamId. */
void dsnd_master_param(dsnd_ctx* ctx, int id, int32_t value);
void dsnd_set_dither(dsnd_ctx* ctx, int enabled);

/* ---- Ducking the music -------------------------------------------------- */

/* 1000 = no duck. A pure function of how long ago each ducking sound fired,
 * so it is reproducible rather than dependent on a level detector.
 *
 * The SFX engine and the music engine are separate instances -- which is what
 * stops a burst of explosions stealing the music's voices -- so this number
 * has to cross the gap by hand: poll it once per audio callback and pass it
 * to msynth_duck_milli(). The music engine does its own ramp, so no smoothing
 * is needed here. */
int dsnd_music_duck_milli(const dsnd_ctx* ctx);

/* ---- Render. Audio thread only. ----------------------------------------- */

/* Interleaved stereo int16 at 48000 Hz. `frames` MUST be a multiple of 64;
 * a partial trailing block is filled with silence rather than rendered, so
 * that the engine never falls half a block out of step with its own clock.
 *
 * There is deliberately no "mix into the host's buffer" variant. Summing two
 * already soft-clipped int16 streams quantises twice and can still clip: sum
 * the SFX and music engines in the host's float graph instead. */
void dsnd_render(dsnd_ctx* ctx, int16_t* out_interleaved, int frames);

/* ---- Diagnostics -------------------------------------------------------- */

typedef struct dsnd_diag {
    int32_t instances;   /* counting against budgets */
    int32_t retiring;    /* fading out; rendered, no longer counted */
    int32_t peak_instances, live_instances, cached_instances;
    int32_t synth_voices, peak_synth_voices;
    int64_t played, stolen;
    int64_t rejected_cooldown, rejected_max_instances, rejected_priority;
    int64_t rejected_bus_budget, rejected_no_voices, rejected_unknown;
    uint64_t queue_dropped;
    uint32_t queue_high_water;
    uint32_t cache_bytes, cached_variants, live_variants;
} dsnd_diag;

void dsnd_diag_get(const dsnd_ctx* ctx, dsnd_diag* out);

/* Absolute engine sample position -- the clock `when` is measured against. */
uint64_t dsnd_now(const dsnd_ctx* ctx);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif  /* DSND_C_API_H */
