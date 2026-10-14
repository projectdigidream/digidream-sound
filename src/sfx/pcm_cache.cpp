#include "pcm_cache.h"

#include <algorithm>

#include "ms.h"

namespace dsnd {
namespace {

// EXACT zero, not "quiet enough".
//
// The obvious choice is a small threshold -- a tail below -74 dB is over by
// any musical standard. It is also wrong here, and subtly: the live path does
// not trim, so anything the cache cuts is a sample the two paths disagree
// about, and the cached-equals-live property dies in the tail where nobody
// looks. Worse, the output stage carries sub-LSB error forward between
// samples, so even a difference below the 16-bit floor does not stay there.
//
// Exact zero is safe because midisynth's envelope stages terminate
// STRUCTURALLY rather than asymptotically: an exp decay of 120 ms is over at
// 120 ms, the voice goes idle, and it then contributes exactly nothing --
// which is precisely what the trimmed samples were.
constexpr i64 SILENCE_LEVEL = 0;
constexpr int SILENT_BLOCKS_TO_END = 4;

// A layer still counting down its startDelayMs is sound still to come, just
// as a gesture note that has not fired is. Silence alone used to end the
// render, so a layer delayed past the end of the others -- a clunk 70 ms
// after a 20 ms click, a tail after a gap -- was cut before it spoke, and
// because the cache is built here, the game never played it either.
bool delayed_layer_pending(const Instance& inst, SfxVoicePool& pool) {
    for (int i = 0; i < pool.capacity(); ++i) {
        if (pool.owner(i) != inst.owner) continue;
        const Voice& v = pool.voice(i);
        if (v.active && v.delay_blocks > 0) return true;
    }
    return false;
}

}  // namespace

bool render_variant(const SoundBank& bank, size_t sound_index, u16 variant_index, bool mono,
                    u32 max_blocks, std::vector<i32>& out, u32* frames_out, bool* leaks_live_out,
                    SfxVoicePool* pool_in) {
    out.clear();
    if (frames_out) *frames_out = 0;
    if (leaks_live_out) *leaks_live_out = false;

    const SoundDef& snd = bank.sounds[sound_index];
    // A caller that renders repeatedly hands its own pool in; anyone else
    // gets one built here and thrown away, which is what a once-per-load
    // cache build wants.
    std::unique_ptr<SfxVoicePool> owned;
    if (!pool_in) {
        owned.reset(new SfxVoicePool());
        pool_in = owned.get();
    }
    SfxVoicePool& pool = *pool_in;
    // A reused pool still holds the previous render's voices, and those point
    // at a bank that no longer exists. Reset rather than release: a fade
    // would render through freed memory, and two independent offline renders
    // share no continuity worth preserving.
    pool.reset();
    const ChannelState ch = neutral_channel();

    Instance inst;
    inst.reset();
    inst.handle = 1;
    inst.active = true;
    inst.sound = static_cast<i32>(sound_index);
    inst.variant = variant_index;
    inst.bus = snd.bus;
    inst.owner = 0;
    inst.live = true;  // the cache is built BY the live path, by definition

    // A fixed seed. The cache is the sound's canonical rendering; per-trigger
    // variation is pitch and gain at playback, not a different noise stream,
    // because a different stream per instance is exactly what cannot be
    // cached.
    inst.seed = mix_seed(0x5EEDu, variant_index, 0, 0);

    // What ends a render is SILENCE, not the voices reporting themselves
    // finished. A patch with `sustain: 0` and no `oneShot` sits in its sustain
    // stage at zero level indefinitely: audibly over, technically still
    // running. Waiting for the voices would render four seconds of digital
    // silence for every such sound and then call it an error.
    //
    // The genuine error -- a variant that never ends -- is therefore "still
    // AUDIBLE at the ceiling", which is what the message says and what a
    // sustaining pad with no note-off actually does.
    const size_t note_count =
        bank.variants[snd.variant_first + variant_index].gesture.size();

    int silent_run = 0;
    bool ended = false;
    bool leaks_live = false;
    for (u32 b = 0; b < max_blocks; ++b) {
        advance_gesture(inst, bank, pool, ch, 0);
        // Asked BEFORE rendering: the block in which a delay reaches zero
        // is itself silent (the voice speaks from the next one), so asking
        // after it would end the render one block too early, every time.
        const bool waiting = delayed_layer_pending(inst, pool);
        const bool alive = render_sources(inst, bank, pool, ch);

        i64 peak = 0;
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            const i64 l = inst.lane_l[i] < 0 ? -inst.lane_l[i] : inst.lane_l[i];
            const i64 r = inst.lane_r[i] < 0 ? -inst.lane_r[i] : inst.lane_r[i];
            if (l > peak) peak = l;
            if (r > peak) peak = r;
            if (mono) {
                // The same sum the spatialiser applies to a live instance on
                // a spatial bus. If these two ever disagree, cached and live
                // stop being the same signal.
                out.push_back(static_cast<i32>(msynth::asr(inst.lane_l[i] + inst.lane_r[i], 1)));
            } else {
                out.push_back(static_cast<i32>(inst.lane_l[i]));
                out.push_back(static_cast<i32>(inst.lane_r[i]));
            }
        }
        pool.reap();
        // The engine advances age after rendering a block, and this must match
        // it exactly: age is what decides when a gesture note fires, so a
        // cache that did not advance it would render only a sound's first note
        // and then wait forever for a second that could never come due.
        ++inst.age_blocks;

        if (peak <= SILENCE_LEVEL) {
            ++silent_run;
            // Every gesture note must have fired first, or a door whose creak
            // arrives 200 ms after its thunk would be cut off at the gap.
            if (silent_run >= SILENT_BLOCKS_TO_END && inst.next_note >= note_count && !waiting) {
                ended = true;
                leaks_live = alive;
                break;
            }
        } else {
            silent_run = 0;
        }
    }

    // Trim the trailing silence we rendered while deciding it had ended.
    const u32 stride = mono ? 1u : 2u;
    while (out.size() >= stride) {
        bool quiet = true;
        for (u32 k = 0; k < stride; ++k) {
            const i64 v = out[out.size() - 1 - k];
            if ((v < 0 ? -v : v) > SILENCE_LEVEL) quiet = false;
        }
        if (!quiet) break;
        out.resize(out.size() - stride);
    }

    if (frames_out) *frames_out = static_cast<u32>(out.size() / stride);
    if (leaks_live_out) *leaks_live_out = leaks_live;
    return ended;
}

LoadResult PcmCache::build(SoundBank& bank, u32 budget_bytes, CacheReport* rep) {
    LoadResult r;
    clear();

    CacheReport local;
    local.budget_bytes = budget_bytes;

    // Highest priority first, then bank order. Deterministic, and it means a
    // budget that runs out sacrifices the sounds that matter least.
    std::vector<size_t> order(bank.sounds.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return bank.sounds[a].priority > bank.sounds[b].priority;
    });

    const u32 max_blocks = ms_to_blocks(static_cast<i32>(MAX_CACHE_MS));

    for (size_t si : order) {
        SoundDef& snd = bank.sounds[si];
        if (snd.live) {
            local.live_variants += snd.variant_count;
            continue;
        }
        const bool mono = bank.buses[snd.bus].spatial;

        // Render every variant before committing any, so a sound never ends
        // up with some variants cached and some live.
        std::vector<std::vector<i32>> rendered(snd.variant_count);
        std::vector<u32> frames(snd.variant_count, 0);
        size_t total = 0;
        bool ok = true;

        for (u16 v = 0; v < snd.variant_count; ++v) {
            if (!render_variant(bank, si, v, mono, max_blocks, rendered[v], &frames[v])) {
                r.error("sound '" + snd.name + "' variant " + std::to_string(v) +
                        " never ends: it is still sounding after " +
                        std::to_string(MAX_CACHE_MS) +
                        " ms. A sustaining amp envelope with no note-off and no loop "
                        "produces a voice that never finishes and an instance that is "
                        "never reclaimed. Give the envelope a decay to zero, give the "
                        "gesture note a durMs, or mark the sound live.");
                ok = false;
                break;
            }
            if (frames[v] == 0) {
                r.warn("sound '" + snd.name + "' variant " + std::to_string(v) +
                       " renders silence");
            }
            // + guard frames
            total += (rendered[v].size() + (mono ? 3u : 6u)) * sizeof(i32);
        }
        if (!ok) return r;

        if (bytes_ + total > budget_bytes) {
            // Over budget: this sound plays live. Keep going rather than
            // stopping, so a smaller high-priority sound further down the
            // list still gets its place.
            snd.live = true;
            local.live_variants += snd.variant_count;
            r.warn("sound '" + snd.name + "': cache budget reached, playing live");
            continue;
        }

        for (u16 v = 0; v < snd.variant_count; ++v) {
            CacheEntry e;
            e.stereo = !mono;
            e.frames = frames[v];
            const size_t lead = mono ? 1u : 2u;
            const size_t tail = mono ? 2u : 4u;
            e.store.assign(lead, 0);
            e.store.insert(e.store.end(), rendered[v].begin(), rendered[v].end());
            e.store.insert(e.store.end(), tail, 0);

            bytes_ += static_cast<u32>(e.bytes());
            bank.variants[snd.variant_first + v].cache_index = static_cast<i32>(entries_.size());
            bank.variants[snd.variant_first + v].duration_blocks =
                (e.frames + CONTROL_BLOCK - 1) / CONTROL_BLOCK;
            entries_.push_back(std::move(e));
            ++local.cached_variants;
        }
    }

    local.bytes = bytes_;
    if (rep) *rep = local;
    return r;
}

}  // namespace dsnd
