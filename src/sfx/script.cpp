#include "script.h"

#include <algorithm>

#include "synth/json_min.h"

namespace dsnd {
namespace {

using msynth::JsonValue;

// Degrees -> brad. 360 degrees is the whole u32, so this is exact at the
// quarter turns and monotone everywhere.
u32 deg_to_brad(i32 deg_q16) {
    const i64 wrapped = ((static_cast<i64>(deg_q16) % (360LL * GAIN_ONE)) + 360LL * GAIN_ONE) %
                        (360LL * GAIN_ONE);
    return static_cast<u32>((wrapped * 4294967296LL) / (360LL * GAIN_ONE));
}

bool read_vec3(const JsonValue* v, i32& x, i32& y, i32& z) {
    if (!v || !v->is_arr() || v->arr.size() < 3) return false;
    x = v->arr[0]->num_q16;
    y = v->arr[1]->num_q16;
    z = v->arr[2]->num_q16;
    return true;
}

}  // namespace

LoadResult load_script(const char* json, size_t len, Script& out) {
    LoadResult r;
    out = Script{};

    std::string err;
    msynth::JsonPtr doc = msynth::json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("script: " + (err.empty() ? std::string("not a JSON object") : err));
        return r;
    }
    if (doc->get_int("version", 1) > 1) {
        r.error("script: version is newer than this build reads");
        return r;
    }
    out.seconds = static_cast<u32>(doc->get_int("seconds", 4));
    if (out.seconds == 0 || out.seconds > 600) {
        r.warn("script: 'seconds' out of range, using 4");
        out.seconds = 4;
    }

    if (const JsonValue* ls = doc->get("listener")) {
        if (ls->is_arr()) {
            for (const auto& e : ls->arr) {
                if (!e->is_obj()) continue;
                ScriptListener k;
                k.at_sample = static_cast<u64>(e->get_int("atSample", 0));
                read_vec3(e->get("pos"), k.x_q16, k.y_q16, k.z_q16);
                k.yaw_brad = deg_to_brad(e->get_q16("yawDeg", 0));
                out.listener.push_back(k);
            }
        }
    }

    const JsonValue* evs = doc->get("events");
    if (!evs || !evs->is_arr()) {
        r.error("script: missing 'events' array");
        return r;
    }
    for (const auto& e : evs->arr) {
        if (!e->is_obj()) continue;
        ScriptEvent ev;
        ev.at_sample = static_cast<u64>(e->get_int("atSample", 0));
        ev.sound = e->get_str("sound", "");
        if (ev.sound.empty()) {
            r.error("script: an event has no 'sound'");
            continue;
        }
        ev.variant = static_cast<i32>(e->get_int("variant", -1));
        ev.seed = static_cast<u32>(e->get_int("seed", 1));
        if (const JsonValue* g = e->get("gainDb"))
            if (g->is_num()) ev.gain_q16 = db_q16_to_gain(g->num_q16);
        ev.has_pos = read_vec3(e->get("pos"), ev.x_q16, ev.y_q16, ev.z_q16);
        ev.pan_q16 = e->get_q16("pan", 0);
        out.events.push_back(std::move(ev));
    }

    // Sorted, because the engine applies commands in queue order and a script
    // written out of order would otherwise render differently from the same
    // script written in order -- which is a difference nobody intends.
    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const ScriptEvent& a, const ScriptEvent& b) {
                         return a.at_sample < b.at_sample;
                     });
    std::stable_sort(out.listener.begin(), out.listener.end(),
                     [](const ScriptListener& a, const ScriptListener& b) {
                         return a.at_sample < b.at_sample;
                     });
    if (out.events.empty()) r.warn("script: no events");
    return r;
}

}  // namespace dsnd
