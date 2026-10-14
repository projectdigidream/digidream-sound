#pragma once
//
// .sfxscript -- a list of triggers at exact sample positions.
//
// This is what makes a sound engine testable. A game's audio is driven by
// events nobody can replay, so the gate is driven by a written-down sequence
// instead: listener keyframes, and triggers with their seeds. Render it, hash
// it, and the same script produces the same bytes on arm64, on x86-64 and
// under wasm, or the build fails.
//
//   { "version": 1, "seconds": 8,
//     "listener": [ { "atSample": 0, "pos": [0,0,1.2], "yawDeg": 0 } ],
//     "events":   [ { "atSample": 0, "sound": "door_open", "variant": 0,
//                     "seed": 12345, "gainDb": 0, "pos": [3,4,0] } ] }
//
// `atSample` is absolute, and the engine's own "apply at or before now" rule
// quantises it to a control block exactly as a gameplay trigger would -- so
// the script exercises the real path, not a special one.
//
#include <string>
#include <vector>

#include "sfx_json.h"
#include "sound_def.h"

namespace dsnd {

struct ScriptEvent {
    u64 at_sample = 0;
    std::string sound;
    i32 variant = -1;  // -1 = choose by weight from the seed
    u32 seed = 1;
    i32 gain_q16 = GAIN_ONE;
    i32 x_q16 = 0, y_q16 = 0, z_q16 = 0;
    i32 pan_q16 = 0;
    bool has_pos = false;
};

struct ScriptListener {
    u64 at_sample = 0;
    i32 x_q16 = 0, y_q16 = 0, z_q16 = 0;
    u32 yaw_brad = 0;
};

struct Script {
    u32 seconds = 4;
    std::vector<ScriptListener> listener;
    std::vector<ScriptEvent> events;
};

LoadResult load_script(const char* json, size_t len, Script& out);

}  // namespace dsnd
