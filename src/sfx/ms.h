#pragma once
//
// Milliseconds -> control blocks, spelled once.
//
// This duplicates `ms_q16_to_blocks` in midisynth's patch_loader.cpp, which is
// `static` in an anonymous namespace and therefore not linkable. Two copies of
// a rounding rule is a real wart, and it is the price of not editing midisynth.
//
// It is a wart with a guard: test_ms.cpp pins this against the patch loader's
// observable behaviour (envelope stage lengths parsed from JSON) at 0, 1,
// 1.333, 60 and 4000 ms. If midisynth ever changes its rule, that test fails
// rather than the two silently disagreeing about how long a decay is.
//
// The rule itself: truncate toward zero, at control-block resolution. A 1 ms
// stage is therefore zero blocks, not one -- shorter than the control rate can
// express is shorter than the engine can render, and rounding it up would make
// a "0.5 ms" click audibly longer than a "1 ms" one.
//
#include "config.h"

namespace dsnd {

// `ms_q16` is Q16.16 milliseconds, the form json_min.h hands back as num_q16.
inline u32 ms_q16_to_blocks(i32 ms_q16) noexcept {
    if (ms_q16 <= 0) return 0;
    return static_cast<u32>((static_cast<i64>(ms_q16) * CONTROL_RATE / 1000) >> GAIN_SHIFT);
}

inline u32 ms_to_blocks(i32 ms) noexcept {
    if (ms <= 0) return 0;
    return static_cast<u32>(static_cast<i64>(ms) * CONTROL_RATE / 1000);
}

inline u32 ms_q16_to_samples(i32 ms_q16) noexcept {
    if (ms_q16 <= 0) return 0;
    return static_cast<u32>((static_cast<i64>(ms_q16) * SAMPLE_RATE / 1000) >> GAIN_SHIFT);
}

inline u32 blocks_to_ms(u32 blocks) noexcept {
    return static_cast<u32>(static_cast<u64>(blocks) * 1000 / CONTROL_RATE);
}

}  // namespace dsnd
