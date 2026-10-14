#include "sfx_json.h"

#include <algorithm>

#include "ms.h"
#include "synth/json_min.h"
#include "synth/song_config.h"

namespace dsnd {
namespace {

using msynth::JsonValue;

// Q16.16 saturates just under 32768, so this is the longest duration the
// JSON number type can carry at all. Anything longer is a typo.
constexpr i32 MAX_MS_Q16 = 32000 * GAIN_ONE;

i32 clampq(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

i64 clampi(i64 v, i64 lo, i64 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A clamp that says so. Silence here is how a bank ends up sounding wrong in
// a way nobody can trace back to the file.
i32 clamp_warn(i32 v, i32 lo, i32 hi, LoadResult& r, const std::string& where) {
    const i32 c = clampq(v, lo, hi);
    if (c != v) r.warn(where + ": out of range, clamped");
    return c;
}

CurveId parse_curve(const std::string& s, LoadResult& r, const std::string& where) {
    if (s.empty() || s == "linear") return CurveId::Linear;
    if (s == "exp") return CurveId::Exp;
    if (s == "expAttack") return CurveId::ExpAttack;
    if (s == "sCurve") return CurveId::SCurve;
    r.warn(where + ": unknown curve '" + s + "', using linear");
    return CurveId::Linear;
}

OnLimit parse_on_limit(const std::string& s, LoadResult& r, const std::string& where) {
    if (s.empty() || s == "reject") return OnLimit::Reject;
    if (s == "stealOldest") return OnLimit::StealOldest;
    r.warn(where + ": unknown onLimit '" + s + "', using reject");
    return OnLimit::Reject;
}

void parse_attenuation(const JsonValue* v, Attenuation& out, LoadResult& r,
                       const std::string& where) {
    if (!v || !v->is_obj()) return;
    const std::string w = where + ".attenuation";

    out.min_dist_q16 = clamp_warn(v->get_q16("minDist", out.min_dist_q16), 0, 4096 * GAIN_ONE, r,
                                  w + ".minDist");
    out.max_dist_q16 = clamp_warn(v->get_q16("maxDist", out.max_dist_q16), 0, 4096 * GAIN_ONE, r,
                                  w + ".maxDist");
    if (out.max_dist_q16 <= out.min_dist_q16) {
        r.warn(w + ": maxDist must exceed minDist, pushing maxDist out");
        out.max_dist_q16 = out.min_dist_q16 + GAIN_ONE;
    }
    out.curve = parse_curve(v->get_str("curve", ""), r, w);
    out.spread_q16 = clamp_warn(v->get_q16("spread", out.spread_q16), 0, GAIN_ONE, r, w + ".spread");

    if (const JsonValue* rd = v->get("rearDb"))
        if (rd->is_num())
            out.rear_gain_q16 = db_q16_to_gain(clamp_warn(rd->num_q16, -96 * GAIN_ONE, 0, r,
                                                          w + ".rearDb"));

    out.air_lpf_hz_q16 =
        clamp_warn(v->get_q16("airLpfHz", out.air_lpf_hz_q16), 0, 20000 * GAIN_ONE, r,
                   w + ".airLpfHz");
}

// A gesture defaults to one note at the start, which is what nearly every
// sound wants. `durMs: 0` means no note-off at all -- the envelope ends the
// voice, which is what `oneShot` patches are for.
void parse_gesture(const JsonValue* v, std::vector<GestureNote>& out, LoadResult& r,
                   const std::string& where) {
    out.clear();
    if (!v || !v->is_arr() || v->arr.empty()) {
        out.push_back(GestureNote{});
        return;
    }
    if (v->arr.size() > MAX_GESTURE_NOTES) {
        r.warn(where + ": more than " + std::to_string(MAX_GESTURE_NOTES) +
               " gesture steps, extra ones dropped (use layers, not steps)");
    }
    const size_t n = std::min<size_t>(v->arr.size(), MAX_GESTURE_NOTES);
    for (size_t i = 0; i < n; ++i) {
        const JsonValue& s = *v->arr[i];
        if (!s.is_obj()) {
            r.warn(where + ": gesture step " + std::to_string(i) + " is not an object, dropped");
            continue;
        }
        GestureNote g;
        g.at_blocks = ms_q16_to_blocks(clamp_warn(s.get_q16("atMs", 0), 0, MAX_MS_Q16, r,
                                                  where + ".atMs"));
        g.dur_blocks = ms_q16_to_blocks(clamp_warn(s.get_q16("durMs", 0), 0, MAX_MS_Q16, r,
                                                   where + ".durMs"));
        g.note = static_cast<u8>(clampi(s.get_int("note", 60), 0, 127));
        g.vel = static_cast<u8>(clampi(s.get_int("vel", 100), 1, 127));

        // phase_inc_for_cents clamps at 13199.99 cents (note 132), and a
        // gesture note near the top plus jitter plus a pitch envelope silently
        // hits that ceiling. Warn while the author can still hear why.
        if (g.note > 108)
            r.warn(where + ": note " + std::to_string(g.note) +
                   " is near the pitch table's ceiling; jitter and pitch envelopes may clamp");
        out.push_back(g);
    }
    if (out.empty()) out.push_back(GestureNote{});
    std::stable_sort(out.begin(), out.end(),
                     [](const GestureNote& a, const GestureNote& b) {
                         return a.at_blocks < b.at_blocks;
                     });
}

}  // namespace

i32 db_q16_to_gain(i32 db_q16) {
    return msynth::db_to_lin_q16(clampq(db_q16, -96 * GAIN_ONE, 24 * GAIN_ONE));
}

LoadResult load_sound(const JsonValue& doc, WaveTableCache& cache, SoundBank& bank, SoundDef& out) {
    LoadResult r;
    if (!doc.is_obj()) {
        r.error("sound: not an object");
        return r;
    }
    out.name = doc.get_str("name", "");
    if (out.name.empty()) {
        r.error("sound: missing 'name'");
        return r;
    }
    const std::string where = "sound '" + out.name + "'";

    const std::string bus = doc.get_str("bus", "");
    if (bus.empty()) {
        out.bus = 0;
    } else {
        const int bi = bank.bus_index(bus);
        if (bi < 0) {
            r.warn(where + ": unknown bus '" + bus + "', using the first");
            out.bus = 0;
        } else {
            out.bus = static_cast<u8>(bi);
        }
    }

    if (const JsonValue* g = doc.get("gainDb"))
        if (g->is_num())
            out.gain_q16 = db_q16_to_gain(clamp_warn(g->num_q16, -96 * GAIN_ONE, 24 * GAIN_ONE, r,
                                                     where + ".gainDb"));

    out.priority = static_cast<u8>(clampi(doc.get_int("priority", 128), 0, 255));
    out.max_instances = static_cast<u8>(clampi(doc.get_int("maxInstances", 4), 1, 64));
    out.on_limit = parse_on_limit(doc.get_str("onLimit", ""), r, where);
    out.cooldown_blocks =
        ms_q16_to_blocks(clamp_warn(doc.get_q16("cooldownMs", 0), 0, MAX_MS_Q16, r,
                                    where + ".cooldownMs"));

    out.pitch_jitter_cents_q16 =
        clamp_warn(doc.get_q16("pitchJitterCents", 0), 0, 2400 * GAIN_ONE, r,
                   where + ".pitchJitterCents");
    out.gain_jitter_db_q16 =
        clamp_warn(doc.get_q16("gainJitterDb", 0), 0, 24 * GAIN_ONE, r, where + ".gainJitterDb");

    out.live = doc.get_bool("live", false);
    out.live_authored = out.live;
    out.loop = doc.get_bool("loop", false);

    // A loop has no end to render, so it cannot be cached as a one-shot.
    if (out.loop && !out.live) {
        out.live = true;
        out.live_authored = true;
        r.warn(where + ": loop implies live (a looping sound has no finite render)");
    }
    // Past the cached rate cap, resampling would alias more than synthesis
    // costs. Say so rather than quietly sounding worse.
    if (!out.live && out.pitch_jitter_cents_q16 > MAX_CACHED_JITTER_CENTS * GAIN_ONE) {
        out.live = true;
        out.live_authored = true;
        r.warn(where + ": pitchJitterCents above " + std::to_string(MAX_CACHED_JITTER_CENTS) +
               " implies live (a cached read would alias)");
    }

    parse_attenuation(doc.get("attenuation"), out.atten, r, where);

    if (const JsonValue* d = doc.get("duckMusic")) {
        if (d->is_obj()) {
            const i32 db = clamp_warn(d->get_q16("db", 0), -48 * GAIN_ONE, 0, r,
                                      where + ".duckMusic.db");
            out.duck_gain_q16 = db_q16_to_gain(db);
            out.duck_blocks = ms_q16_to_blocks(
                clamp_warn(d->get_q16("ms", 0), 0, MAX_MS_Q16, r, where + ".duckMusic.ms"));
            if (out.duck_blocks == 0) out.duck_gain_q16 = GAIN_ONE;  // no duration, no duck
        }
    }

    // The modal body: an exciter (the patch) feeding a bank of damped
    // resonators. `resonators` is the list of modes, `resonatorDry` how much
    // of the exciter survives under them.
    if (const JsonValue* rs = doc.get("resonators")) {
        if (rs->is_arr()) {
            for (const auto& mj : rs->arr) {
                if (!mj->is_obj()) continue;
                if (out.resonators.count >= MAX_RESONATORS) {
                    r.warn(where + ": more than " + std::to_string(MAX_RESONATORS) +
                           " resonators, extra dropped");
                    break;
                }
                ResonatorDef d;
                d.hz_q16 = clamp_warn(mj->get_q16("hz", 0), 20 << GAIN_SHIFT,
                                      12000 << GAIN_SHIFT, r, where + ".resonators.hz");
                d.decay_ms_q16 = clamp_warn(mj->get_q16("decayMs", 0), 1 << GAIN_SHIFT,
                                            4000 << GAIN_SHIFT, r, where + ".resonators.decayMs");
                d.gain_q16 = clamp_warn(mj->get_q16("gain", GAIN_ONE), 0, 4 * GAIN_ONE, r,
                                        where + ".resonators.gain");
                out.resonators.modes[out.resonators.count++] = d;
            }
        } else {
            r.warn(where + ": 'resonators' is not an array, ignored");
        }
    }
    if (const JsonValue* dv = doc.get("resonatorDry"))
        if (dv->is_num())
            out.resonators.dry_q16 = clamp_warn(dv->num_q16, 0, 2 * GAIN_ONE, r,
                                                where + ".resonatorDry");

    if (const JsonValue* t = doc.get("tags"))
        if (t->is_arr())
            for (const auto& e : t->arr)
                if (e->is_str()) out.tags.push_back(e->str);

    // ---- variants ----
    const JsonValue* vars = doc.get("variants");
    if (!vars || !vars->is_arr() || vars->arr.empty()) {
        r.error(where + ": needs at least one variant");
        return r;
    }
    out.variant_first = static_cast<u16>(bank.variants.size());
    for (size_t i = 0; i < vars->arr.size(); ++i) {
        const JsonValue& vj = *vars->arr[i];
        const std::string vw = where + " variant " + std::to_string(i);
        if (!vj.is_obj()) {
            r.error(vw + ": not an object");
            continue;
        }
        Variant v;
        v.weight = static_cast<u16>(clampi(vj.get_int("weight", 1), 1, 1000));

        const JsonValue* pj = vj.get("patch");
        if (!pj) {
            r.error(vw + ": missing 'patch'");
            continue;
        }
        auto patch = std::make_unique<Patch>();
        LoadResult pr = msynth::load_patch(*pj, cache, *patch);
        for (const auto& w : pr.warnings) r.warn(vw + ": " + w);
        for (const auto& e : pr.errors) r.error(vw + ": " + e);
        if (!pr.ok) continue;
        if (patch->name.empty()) patch->name = out.name;

        // Dedupe by serialized form. Two variants of a door that differ only
        // in their gesture share one patch, and a bank that reuses a family
        // recipe across forty roles stores it once.
        const std::string text = msynth::json_serialize(*pj);
        int found = -1;
        for (size_t k = 0; k < bank.patch_json.size(); ++k)
            if (bank.patch_json[k] == text) { found = static_cast<int>(k); break; }

        if (found >= 0) {
            v.patch_index = static_cast<u16>(found);
        } else {
            v.patch_index = static_cast<u16>(bank.patches.size());
            bank.patches.push_back(std::move(patch));
            bank.patch_json.push_back(text);
        }
        parse_gesture(vj.get("gesture"), v.gesture, r, vw);

        // A voice is released by a note-off or by an envelope that decays to
        // zero. A layer with neither has no way to end, and the instance
        // holding it is never reclaimed -- so the sound plays a few times and
        // then that slot is gone for the rest of the session.
        //
        // This is a static property of the patch and the gesture, so it is
        // caught here rather than by watching a render: a render can only see
        // that something is still quietly sounding, which a long decay also
        // looks like.
        // A loop is meant to run until something stops it, so a sustaining
        // envelope is exactly right there and the check does not apply.
        const msynth::Patch& chk = *bank.patches[v.patch_index];
        for (u8 li = 0; li < chk.layer_count && !out.loop; ++li) {
            if (chk.one_shot || chk.layers[li].amp_env.one_shot) continue;
            if (chk.layers[li].amp_env.sustain == 0) continue;
            bool released = false;
            for (const auto& g : v.gesture)
                if (g.dur_blocks > 0) released = true;
            if (released) continue;
            r.error(vw + ": layer " + std::to_string(li) +
                    " sustains above zero and is never released, so its voice can never "
                    "finish and this sound would leak an instance every time it played. "
                    "Add \"oneShot\": true to the patch, give the amp envelope "
                    "\"sustain\": 0, or give the gesture note a \"durMs\".");
        }

        bank.variants.push_back(std::move(v));
    }
    out.variant_count = static_cast<u16>(bank.variants.size() - out.variant_first);
    if (out.variant_count == 0) r.error(where + ": every variant failed to load");
    return r;
}

LoadResult load_bank_json(const char* json, size_t len, WaveTableCache& cache, SoundBank& out) {
    LoadResult r;
    std::string err;
    msynth::JsonPtr doc = msynth::json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("bank: " + (err.empty() ? std::string("not a JSON object") : err));
        return r;
    }

    const i64 version = doc->get_int("version", 1);
    if (version > 1) {
        r.error("bank: version " + std::to_string(version) + " is newer than this build reads");
        return r;
    }
    out.name = doc->get_str("name", "untitled");

    // ---- buses ----
    if (const JsonValue* bs = doc->get("buses")) {
        if (bs->is_arr()) {
            for (const auto& bj : bs->arr) {
                if (!bj->is_obj()) continue;
                if (out.buses.size() >= MAX_BUSES) {
                    r.warn("bank: more than " + std::to_string(MAX_BUSES) + " buses, extra dropped");
                    break;
                }
                BusDef b;
                b.name = bj->get_str("name", "bus" + std::to_string(out.buses.size()));
                if (const JsonValue* g = bj->get("gainDb"))
                    if (g->is_num()) b.gain_q16 = db_q16_to_gain(g->num_q16);
                b.max_instances = static_cast<u16>(clampi(bj->get_int("maxInstances", 16), 1,
                                                          MAX_SFX_INSTANCES));
                b.spatial = bj->get_bool("spatial", true);
                b.send_reverb_q16 = clamp_warn(bj->get_q16("sendReverb", 0), 0, GAIN_ONE, r,
                                               "bus '" + b.name + "'.sendReverb");
                b.send_chorus_q16 = clamp_warn(bj->get_q16("sendChorus", 0), 0, GAIN_ONE, r,
                                               "bus '" + b.name + "'.sendChorus");
                b.send_delay_q16 = clamp_warn(bj->get_q16("sendDelay", 0), 0, GAIN_ONE, r,
                                              "bus '" + b.name + "'.sendDelay");
                out.buses.push_back(std::move(b));
            }
        } else {
            r.warn("bank: 'buses' is not an array, using one default bus");
        }
    }
    if (out.buses.empty()) {
        BusDef b;
        b.name = "sfx";
        out.buses.push_back(std::move(b));
    }

    // ---- fx and master ----
    //
    // Re-serialized and handed to midisynth's own loaders rather than parsed
    // here. It costs one round trip at load time and buys the guarantee that
    // `fx` and `master` in a .sfxproj mean exactly what they mean in an
    // fx.json -- a second parser for the same schema would drift, and the
    // drift would show up as a reverb that sounds different in two tools.
    if (const JsonValue* fx = doc->get("fx")) {
        if (fx->is_obj()) {
            const std::string text = msynth::json_serialize(*fx);
            msynth::SongFxConfig cfg;
            LoadResult fr = msynth::load_song_config(text.c_str(), text.size(), cfg);
            for (const auto& w : fr.warnings) r.warn("fx: " + w);
            for (const auto& e : fr.errors) r.error("fx: " + e);
            out.reverb = cfg.reverb;
            out.chorus = cfg.chorus;
            out.delay = cfg.delay;
        }
    }
    if (const JsonValue* m = doc->get("master")) {
        if (m->is_obj()) {
            const std::string text = msynth::json_serialize(*m);
            LoadResult mr = msynth::load_master_config(text.c_str(), text.size(), out.master);
            for (const auto& w : mr.warnings) r.warn("master: " + w);
            for (const auto& e : mr.errors) r.error("master: " + e);
        }
    }

    // ---- sounds ----
    const JsonValue* sounds = doc->get("sounds");
    if (!sounds || !sounds->is_arr()) {
        r.error("bank: missing 'sounds' array");
        return r;
    }
    for (const auto& sj : sounds->arr) {
        SoundDef s;
        LoadResult sr = load_sound(*sj, cache, out, s);
        for (const auto& w : sr.warnings) r.warn(w);
        for (const auto& e : sr.errors) r.error(e);
        if (!sr.ok) continue;
        if (out.sound_index(s.name) >= 0) {
            r.error("sound '" + s.name + "': duplicate name");
            continue;
        }
        out.sounds.push_back(std::move(s));
    }
    if (out.sounds.empty()) r.error("bank: no sound loaded");
    return r;
}

}  // namespace dsnd
