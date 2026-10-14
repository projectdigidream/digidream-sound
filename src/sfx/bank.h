#pragma once
//
// .sfxb -- the baked, sealed sound bank.
//
// Framed exactly like midisynth's .mstk, deliberately: magic, a u16 version, a
// sha256 of the payload, then a section table of {tag, offset, size}. Unknown
// sections are ignored so a newer baker can add one, a newer MAJOR version is
// refused outright, and a payload whose hash does not match is refused whole
// rather than half-read. Those three rules are what let the format grow
// without a flag day, and they are worth copying rather than inventing.
//
// What is NOT copied from .mstk is the idea of baking audio in. A .sfxb
// carries recipes and policy, never PCM. Baking the rendered sound would
// multiply the file by ~100x, pin the shipped audio to one build of voice.cpp
// (so a DSP fix would silently ship stale sound), and duplicate work the
// runtime has to be able to do anyway for `live` sounds. What META carries
// instead is a render seal: the sha256 of a reference .sfxscript render, which
// `dsound_bake --check` re-verifies on any machine.
//
// Sections:
//   BUSS   buses: name, gain, budget, spatial flag, reverb send
//   SNDS   sounds: the resolved playback policy, in fixed point
//   VARS   variants: patch index, weight, gesture
//   PTCH   patch JSON, deduped and length-prefixed
//   FX     reverb parameters
//   MSTR   mastering parameters, written only when enabled
//   META   name, counts, the render seal, the content hash
//
#include <string>
#include <vector>

#include "sfx_json.h"
#include "sound_def.h"

namespace dsnd {

inline constexpr u32 SFXB_MAGIC = 0x42584653u;  // "SFXB" little-endian
inline constexpr u16 SFXB_VERSION = 1;

struct BankMeta {
    std::string name;
    u32 sound_count = 0;
    u32 variant_count = 0;
    u32 patch_count = 0;
    bool has_render_hash = false;
    u8 render_hash[32] = {};
    std::string render_script;
    u8 content_hash[32] = {};  // sha256 of the patch JSON, in order
};

// .sfxproj text -> .sfxb bytes. Parses and validates on the way through, so a
// bank that would not load does not bake.
LoadResult bake_bank(const char* json, size_t len, std::vector<u8>& out);

// .sfxb bytes -> SoundBank. `meta` is optional.
LoadResult load_bank(const u8* data, size_t len, WaveTableCache& cache, SoundBank& out,
                     BankMeta* meta = nullptr);

// Read only the header and META, without building any patch. For `--check`
// and for a tool listing banks it has not opened.
LoadResult read_bank_meta(const u8* data, size_t len, BankMeta& out);

// Stamp a render seal into an already-baked bank, in place.
bool seal_bank(std::vector<u8>& bank, const u8 hash[32], const std::string& script_name);

}  // namespace dsnd
