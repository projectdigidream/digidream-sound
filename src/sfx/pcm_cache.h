#pragma once
//
// The PCM cache: every non-`live` variant rendered once, at load.
//
// Storage is Q8.24 i32 -- the engine's own sample format, not i16. That costs
// twice the memory and it is not negotiable: a lossy cache would still be
// deterministic, but it would no longer be IDENTICAL to the live render, and
// the identity is what licenses everything else here (see sample_voice.h).
// If the budget ever proves too tight, the levers are maxCacheMs and `live`,
// never the sample format.
//
// Built once, then immutable. There is deliberately no runtime eviction: an
// LRU would make which path a sound takes depend on play history, so the same
// trigger would sometimes allocate a synth voice and sometimes not, and voice
// limiting would stop being reproducible from one run to the next. A golden
// that cannot be reproduced is not a golden.
//
// Mono for spatial buses, stereo otherwise. A spatial source is a point in
// the world and its own stereo image would fight the panner; the baker warns
// when it discards a non-zero one, because otherwise the author just hears
// their wide sweep go narrow and has nowhere to look.
//
#include <vector>

#include "instance.h"
#include "sfx_json.h"
#include "sound_def.h"

namespace dsnd {

struct CacheEntry {
    // Guard samples are physically present -- one frame before, two after --
    // so the interpolator's inner loop needs no bounds branch.
    std::vector<i32> store;
    u32 frames = 0;
    bool stereo = false;

    const i32* data() const { return store.data() + (stereo ? 2 : 1); }
    size_t bytes() const { return store.size() * sizeof(i32); }
};

struct CacheReport {
    u32 cached_variants = 0;
    u32 live_variants = 0;
    u32 bytes = 0;
    u32 budget_bytes = 0;
};

class PcmCache {
  public:
    // Render every eligible variant, in (priority desc, index asc) order,
    // until the budget is reached. A variant that would exceed it makes its
    // WHOLE SOUND live -- variants of one sound never split across modes,
    // because two variants of a door that take different code paths are two
    // different doors to reason about.
    //
    // The result depends only on the sorted order and the rendered sizes, so
    // it is the same on every machine and every run.
    LoadResult build(SoundBank& bank, u32 budget_bytes, CacheReport* rep = nullptr);

    const CacheEntry& entry(size_t i) const { return entries_[i]; }
    size_t size() const { return entries_.size(); }
    u32 bytes() const { return bytes_; }

    void clear() {
        entries_.clear();
        bytes_ = 0;
    }

  private:
    std::vector<CacheEntry> entries_;
    u32 bytes_ = 0;
};

// Render one variant in isolation, exactly as a live instance would, and
// return its lane. `mono` sums the two lanes the same way the spatialiser
// does, so a cached entry and a live instance agree sample for sample.
//
// Returns false if the variant had not fallen silent by `max_blocks` -- which
// almost always means a sustaining envelope with no note-off, an instance that
// would never end and never be reclaimed. The baker turns that into an error
// while someone can still fix it.
// `leaks_live_out`, when given, reports a variant that falls silent but whose
// voices never finish -- harmless cached, a leak if the sound were made live.
// `pool`, when given, is reused instead of being constructed per call. A
// SfxVoicePool is 48 voices of ~17 KB each -- 820 KB built and torn down every
// time -- which is irrelevant when a bank is cached once at load and
// completely dominant when a fitter renders it ten thousand times.
bool render_variant(const SoundBank& bank, size_t sound_index, u16 variant_index, bool mono,
                    u32 max_blocks, std::vector<i32>& out, u32* frames_out,
                    bool* leaks_live_out = nullptr, SfxVoicePool* pool = nullptr);

}  // namespace dsnd
