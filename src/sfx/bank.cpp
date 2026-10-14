#include "bank.h"

#include <cstring>

#include "output/sha256.h"
#include "synth/json_min.h"

namespace dsnd {
namespace {

// ---- little-endian byte IO -------------------------------------------------
// Deliberately the same shape as midisynth's track.cpp. Abstracting one
// container over both projects would mean exporting it from midisynth, which
// is an edit to a file its goldens depend on; seventy lines of duplication is
// the cheaper side of that trade, and the format they write is the contract
// that keeps them honest.
struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u16 v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void blob(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
    void str(const std::string& s) {
        u32v(static_cast<u32>(s.size()));
        blob(s.data(), s.size());
    }
};

struct Reader {
    const u8* p;
    const u8* end;
    bool ok = true;

    bool need(size_t n) {
        if (static_cast<size_t>(end - p) < n) { ok = false; return false; }
        return true;
    }
    size_t remaining() const { return static_cast<size_t>(end - p); }
    u8 u8v() { if (!need(1)) return 0; return *p++; }
    u16 u16v() {
        if (!need(2)) return 0;
        const u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    i32 i32v() { return static_cast<i32>(u32v()); }
    std::string str(u32 max_len) {
        const u32 n = u32v();
        if (n > max_len || !need(n)) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

constexpr u32 tag4(const char (&s)[5]) {
    return static_cast<u32>(static_cast<u8>(s[0])) |
           (static_cast<u32>(static_cast<u8>(s[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(s[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(s[3])) << 24);
}

constexpr u32 TAG_BUSS = tag4("BUSS");
constexpr u32 TAG_SNDS = tag4("SNDS");
constexpr u32 TAG_VARS = tag4("VARS");
constexpr u32 TAG_PTCH = tag4("PTCH");
constexpr u32 TAG_FX = tag4("FX  ");
// Per-bus chorus and delay sends.
//
// A NEW SECTION rather than three more fields on BUSS, because BUSS records
// are packed with no length prefix: appending to them would make every bank
// written before today unreadable, and this build is allowed to read those
// (the version gate only refuses banks NEWER than it). Unknown sections are
// ignored by design -- docs/SFX_FORMAT.md 6 -- so an old reader skips this and
// a new reader treats its absence as "all sends zero", which is what an old
// bank meant.
constexpr u32 TAG_BSND = tag4("BSND");
// Per-sound modal resonator banks. Optional, for the same reason BSND is: a
// bank that rings nothing bakes to exactly the bytes it did before this
// existed, and an older reader skips a section it does not know.
constexpr u32 TAG_RESO = tag4("RESO");
constexpr u32 TAG_MSTR = tag4("MSTR");
constexpr u32 TAG_META = tag4("META");

constexpr u32 MAX_SECTIONS = 64;
constexpr u32 MAX_STR = 1u << 20;

void hex_to_bytes(const std::string& hex, u8 out[32]) {
    auto nib = [](char c) -> u8 {
        if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
        return 0;
    };
    for (int i = 0; i < 32; ++i)
        out[i] = static_cast<u8>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
}

struct Section {
    u32 tag;
    std::vector<u8> body;
};

void write_buses(const SoundBank& b, std::vector<u8>& out) {
    Writer w{out};
    w.u32v(static_cast<u32>(b.buses.size()));
    for (const auto& bus : b.buses) {
        w.str(bus.name);
        w.i32v(bus.gain_q16);
        w.u16v(bus.max_instances);
        w.u8v(bus.spatial ? 1 : 0);
        w.i32v(bus.send_reverb_q16);
    }
}

// Only written when something is non-zero, so a bank that uses neither effect
// bakes to exactly the bytes it did before this existed.
bool write_bus_sends(const SoundBank& b, std::vector<u8>& out) {
    bool any = false;
    for (const auto& bus : b.buses)
        if (bus.send_chorus_q16 || bus.send_delay_q16) any = true;
    if (!any) return false;
    Writer w{out};
    w.u32v(static_cast<u32>(b.buses.size()));
    for (const auto& bus : b.buses) {
        w.i32v(bus.send_chorus_q16);
        w.i32v(bus.send_delay_q16);
    }
    return true;
}

bool write_resonators(const SoundBank& b, std::vector<u8>& out) {
    bool any = false;
    for (const auto& s : b.sounds) if (!s.resonators.empty()) any = true;
    if (!any) return false;
    Writer w{out};
    w.u32v(static_cast<u32>(b.sounds.size()));
    for (const auto& s : b.sounds) {
        w.u8v(s.resonators.count);
        w.i32v(s.resonators.dry_q16);
        for (u8 i = 0; i < s.resonators.count; ++i) {
            w.i32v(s.resonators.modes[i].hz_q16);
            w.i32v(s.resonators.modes[i].decay_ms_q16);
            w.i32v(s.resonators.modes[i].gain_q16);
        }
    }
    return true;
}

void write_sounds(const SoundBank& b, std::vector<u8>& out) {
    Writer w{out};
    w.u32v(static_cast<u32>(b.sounds.size()));
    for (const auto& s : b.sounds) {
        w.str(s.name);
        w.u8v(s.bus);
        w.i32v(s.gain_q16);
        w.u8v(s.priority);
        w.u8v(s.max_instances);
        w.u8v(static_cast<u8>(s.on_limit));
        w.u32v(s.cooldown_blocks);
        w.i32v(s.pitch_jitter_cents_q16);
        w.i32v(s.gain_jitter_db_q16);
        w.u8v(static_cast<u8>((s.live ? 1 : 0) | (s.loop ? 2 : 0) | (s.live_authored ? 4 : 0)));
        w.i32v(s.atten.min_dist_q16);
        w.i32v(s.atten.max_dist_q16);
        w.u8v(static_cast<u8>(s.atten.curve));
        w.i32v(s.atten.spread_q16);
        w.i32v(s.atten.rear_gain_q16);
        w.i32v(s.atten.air_lpf_hz_q16);
        w.i32v(s.duck_gain_q16);
        w.u32v(s.duck_blocks);
        w.u16v(s.variant_first);
        w.u16v(s.variant_count);
        // `tags` are deliberately not written: they steer the editor's grid
        // and mean nothing at runtime.
    }
}

void write_variants(const SoundBank& b, std::vector<u8>& out) {
    Writer w{out};
    w.u32v(static_cast<u32>(b.variants.size()));
    for (const auto& v : b.variants) {
        w.u16v(v.patch_index);
        w.u16v(v.weight);
        w.u8v(static_cast<u8>(v.gesture.size()));
        for (const auto& g : v.gesture) {
            w.u32v(g.at_blocks);
            w.u32v(g.dur_blocks);
            w.u8v(g.note);
            w.u8v(g.vel);
        }
    }
}

void write_fx(const SoundBank& b, std::vector<u8>& out) {
    Writer w{out};
    w.i32v(b.reverb.room_size);
    w.i32v(b.reverb.damping);
    w.i32v(b.reverb.width);
    w.i32v(b.reverb.wet_gain);
    w.i32v(b.reverb.pre_delay_ms_q16);
    w.i32v(b.reverb.high_pass_hz_q16);
    // The append-only tail continues: chorus, then delay.
    w.i32v(b.chorus.rate_hz_q16);
    w.i32v(b.chorus.depth_ms_q16);
    w.i32v(b.chorus.wet_gain);
    w.i32v(b.delay.time_ms_q16);
    w.i32v(b.delay.feedback_q16);
    w.i32v(b.delay.wet_gain);
    w.u8v(b.delay.ping_pong ? 1 : 0);
    w.i32v(b.delay.sync_beats_q16);
}

void read_fx(Reader& r, SoundBank& b) {
    b.reverb.room_size = r.i32v();
    b.reverb.damping = r.i32v();
    b.reverb.width = r.i32v();
    b.reverb.wet_gain = r.i32v();
    // Append-only tail: an older bank simply stops here.
    if (r.remaining() >= 8) {
        b.reverb.pre_delay_ms_q16 = r.i32v();
        b.reverb.high_pass_hz_q16 = r.i32v();
    }
    if (r.remaining() >= 6 * 4 + 1) {
        b.chorus.rate_hz_q16 = r.i32v();
        b.chorus.depth_ms_q16 = r.i32v();
        b.chorus.wet_gain = r.i32v();
        b.delay.time_ms_q16 = r.i32v();
        b.delay.feedback_q16 = r.i32v();
        b.delay.wet_gain = r.i32v();
        b.delay.ping_pong = r.u8v() != 0;
        b.delay.sync_beats_q16 = r.i32v();
    }
}

void write_master(const SoundBank& b, std::vector<u8>& out) {
    Writer w{out};
    const u32 n = static_cast<u32>(msynth::MasterParamId::COUNT);
    w.u32v(n);
    for (u32 i = 0; i < n; ++i) w.i32v(b.master.get(static_cast<msynth::MasterParamId>(i)));
}

void read_master(Reader& r, SoundBank& b) {
    const u32 n = r.u32v();
    const u32 known = static_cast<u32>(msynth::MasterParamId::COUNT);
    for (u32 i = 0; i < n && r.ok; ++i) {
        const i32 v = r.i32v();
        // Append-only: a newer bank's extra parameters are read past and
        // ignored rather than misassigned.
        if (i < known) b.master.set(static_cast<msynth::MasterParamId>(i), v);
    }
}

}  // namespace

LoadResult bake_bank(const char* json, size_t len, std::vector<u8>& out) {
    out.clear();
    WaveTableCache cache;
    SoundBank bank;
    LoadResult rep = load_bank_json(json, len, cache, bank);
    if (!rep.ok) return rep;

    std::vector<Section> secs;
    secs.push_back({TAG_BUSS, {}});
    write_buses(bank, secs.back().body);
    {
        Section bsnd{TAG_BSND, {}};
        if (write_bus_sends(bank, bsnd.body)) secs.push_back(std::move(bsnd));
    }
    {
        Section reso{TAG_RESO, {}};
        if (write_resonators(bank, reso.body)) secs.push_back(std::move(reso));
    }
    secs.push_back({TAG_SNDS, {}});
    write_sounds(bank, secs.back().body);
    secs.push_back({TAG_VARS, {}});
    write_variants(bank, secs.back().body);

    secs.push_back({TAG_PTCH, {}});
    {
        Writer w{secs.back().body};
        w.u32v(static_cast<u32>(bank.patch_json.size()));
        for (const auto& p : bank.patch_json) w.str(p);
    }

    secs.push_back({TAG_FX, {}});
    write_fx(bank, secs.back().body);

    if (bank.master.enabled) {
        secs.push_back({TAG_MSTR, {}});
        write_master(bank, secs.back().body);
    }

    // content_hash identifies the *sound* of a bank: the patch JSON, in order.
    // Policy changes (a cooldown, a priority) do not move it, which is what
    // makes it usable for "can these two banks share a cache".
    msynth::Sha256 ch;
    for (const auto& p : bank.patch_json) ch.update(p.data(), p.size());
    u8 content[32];
    hex_to_bytes(ch.hex_digest(), content);

    secs.push_back({TAG_META, {}});
    {
        Writer w{secs.back().body};
        w.str(bank.name);
        w.u32v(static_cast<u32>(bank.sounds.size()));
        w.u32v(static_cast<u32>(bank.variants.size()));
        w.u32v(static_cast<u32>(bank.patch_json.size()));
        w.u8v(0);  // has_render_hash; seal_bank() flips it
        u8 zero[32] = {};
        w.blob(zero, 32);
        w.str("");  // render script name
        w.blob(content, 32);
    }

    std::vector<u8> payload;
    struct Ent { u32 tag, off, size; };
    std::vector<Ent> table;
    for (const auto& s : secs) {
        table.push_back({s.tag, static_cast<u32>(payload.size()), static_cast<u32>(s.body.size())});
        payload.insert(payload.end(), s.body.begin(), s.body.end());
    }

    Writer hw{out};
    hw.u32v(SFXB_MAGIC);
    hw.u16v(SFXB_VERSION);
    hw.u16v(0);  // flags
    const std::string hash = msynth::sha256_hex(payload.data(), payload.size());
    hw.blob(hash.data(), 64);
    hw.u32v(static_cast<u32>(table.size()));
    for (const auto& e : table) {
        hw.u32v(e.tag);
        hw.u32v(e.off);
        hw.u32v(e.size);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return rep;
}

namespace {

// Shared by load_bank and read_bank_meta: validates the frame and hands back
// the payload plus the section table.
struct Frame {
    const u8* payload = nullptr;
    size_t payload_len = 0;
    struct Ent { u32 tag, off, size; };
    std::vector<Ent> table;

    const Ent* find(u32 tag) const {
        for (const auto& e : table)
            if (e.tag == tag) return &e;
        return nullptr;
    }
    Reader reader(const Ent* e) const {
        return Reader{payload + e->off, payload + e->off + e->size};
    }
};

bool open_frame(const u8* data, size_t len, Frame& f, LoadResult& rep) {
    Reader r{data, data + len};
    if (r.u32v() != SFXB_MAGIC) {
        rep.error("not a sound bank (bad magic)");
        return false;
    }
    const u16 version = r.u16v();
    r.u16v();  // flags
    if (version > SFXB_VERSION) {
        rep.error("bank version " + std::to_string(version) + " is newer than this build reads");
        return false;
    }
    if (!r.need(64)) {
        rep.error("truncated header");
        return false;
    }
    const std::string stored(reinterpret_cast<const char*>(r.p), 64);
    r.p += 64;
    const u32 nsecs = r.u32v();
    if (!r.ok || nsecs > MAX_SECTIONS) {
        rep.error("bad section table");
        return false;
    }
    f.table.resize(nsecs);
    for (auto& e : f.table) {
        e.tag = r.u32v();
        e.off = r.u32v();
        e.size = r.u32v();
    }
    if (!r.ok) {
        rep.error("truncated section table");
        return false;
    }
    f.payload = r.p;
    f.payload_len = static_cast<size_t>(data + len - r.p);

    // Trusted or rejected. A bank that half-verifies is a bank that plays
    // something nobody authored.
    if (msynth::sha256_hex(f.payload, f.payload_len) != stored) {
        rep.error("payload hash mismatch (corrupt bank)");
        return false;
    }
    for (const auto& e : f.table) {
        if (static_cast<u64>(e.off) + e.size > f.payload_len) {
            rep.error("section out of bounds");
            return false;
        }
    }
    return true;
}

void read_meta(Reader& r, BankMeta& m) {
    m.name = r.str(MAX_STR);
    m.sound_count = r.u32v();
    m.variant_count = r.u32v();
    m.patch_count = r.u32v();
    m.has_render_hash = r.u8v() != 0;
    if (r.need(32)) {
        std::memcpy(m.render_hash, r.p, 32);
        r.p += 32;
    }
    m.render_script = r.str(MAX_STR);
    if (r.need(32)) {
        std::memcpy(m.content_hash, r.p, 32);
        r.p += 32;
    }
}

}  // namespace

LoadResult read_bank_meta(const u8* data, size_t len, BankMeta& out) {
    LoadResult rep;
    out = BankMeta{};
    Frame f;
    if (!open_frame(data, len, f, rep)) return rep;
    const Frame::Ent* e = f.find(TAG_META);
    if (!e) {
        rep.error("missing META section");
        return rep;
    }
    Reader r = f.reader(e);
    read_meta(r, out);
    if (!r.ok) rep.error("truncated META section");
    return rep;
}

LoadResult load_bank(const u8* data, size_t len, WaveTableCache& cache, SoundBank& out,
                     BankMeta* meta) {
    LoadResult rep;
    out = SoundBank{};
    Frame f;
    if (!open_frame(data, len, f, rep)) return rep;

    const Frame::Ent* buss = f.find(TAG_BUSS);
    const Frame::Ent* snds = f.find(TAG_SNDS);
    const Frame::Ent* vars = f.find(TAG_VARS);
    const Frame::Ent* ptch = f.find(TAG_PTCH);
    if (!buss || !snds || !vars || !ptch) {
        rep.error("missing required section");
        return rep;
    }

    {
        Reader r = f.reader(buss);
        const u32 n = r.u32v();
        if (n > MAX_BUSES) {
            rep.error("bank declares more buses than this build supports");
            return rep;
        }
        for (u32 i = 0; i < n && r.ok; ++i) {
            BusDef b;
            b.name = r.str(MAX_STR);
            b.gain_q16 = r.i32v();
            b.max_instances = r.u16v();
            b.spatial = r.u8v() != 0;
            b.send_reverb_q16 = r.i32v();
            out.buses.push_back(std::move(b));
        }
        if (!r.ok) {
            rep.error("truncated BUSS section");
            return rep;
        }
    }

    // Optional. Absent means every send is zero, which is what every bank
    // written before this section existed meant.
    if (const Frame::Ent* bsnd = f.find(TAG_BSND)) {
        Reader r = f.reader(bsnd);
        const u32 n = r.u32v();
        for (u32 i = 0; i < n && r.ok; ++i) {
            const i32 cho = r.i32v();
            const i32 dly = r.i32v();
            if (i < out.buses.size()) {
                out.buses[i].send_chorus_q16 = cho;
                out.buses[i].send_delay_q16 = dly;
            }
        }
        if (!r.ok) {
            rep.error("truncated BSND section");
            return rep;
        }
    }

    {
        Reader r = f.reader(ptch);
        const u32 n = r.u32v();
        for (u32 i = 0; i < n && r.ok; ++i) out.patch_json.push_back(r.str(MAX_STR));
        if (!r.ok) {
            rep.error("truncated PTCH section");
            return rep;
        }
        for (size_t i = 0; i < out.patch_json.size(); ++i) {
            std::string err;
            msynth::JsonPtr doc =
                msynth::json_parse(out.patch_json[i].data(), out.patch_json[i].size(), &err);
            auto patch = std::make_unique<Patch>();
            if (!doc) {
                rep.error("patch " + std::to_string(i) + ": " + err);
                return rep;
            }
            LoadResult pr = msynth::load_patch(*doc, cache, *patch);
            for (const auto& w : pr.warnings) rep.warn("patch " + std::to_string(i) + ": " + w);
            for (const auto& e : pr.errors) rep.error("patch " + std::to_string(i) + ": " + e);
            if (!pr.ok) return rep;
            out.patches.push_back(std::move(patch));
        }
    }

    {
        Reader r = f.reader(vars);
        const u32 n = r.u32v();
        for (u32 i = 0; i < n && r.ok; ++i) {
            Variant v;
            v.patch_index = r.u16v();
            v.weight = r.u16v();
            const u8 gn = r.u8v();
            if (gn > MAX_GESTURE_NOTES) {
                rep.error("variant " + std::to_string(i) + ": gesture too long");
                return rep;
            }
            for (u8 k = 0; k < gn && r.ok; ++k) {
                GestureNote g;
                g.at_blocks = r.u32v();
                g.dur_blocks = r.u32v();
                g.note = r.u8v();
                g.vel = r.u8v();
                v.gesture.push_back(g);
            }
            if (v.patch_index >= out.patches.size()) {
                rep.error("variant " + std::to_string(i) + ": patch index out of range");
                return rep;
            }
            out.variants.push_back(std::move(v));
        }
        if (!r.ok) {
            rep.error("truncated VARS section");
            return rep;
        }
    }

    {
        Reader r = f.reader(snds);
        const u32 n = r.u32v();
        for (u32 i = 0; i < n && r.ok; ++i) {
            SoundDef s;
            s.name = r.str(MAX_STR);
            s.bus = r.u8v();
            s.gain_q16 = r.i32v();
            s.priority = r.u8v();
            s.max_instances = r.u8v();
            s.on_limit = static_cast<OnLimit>(r.u8v());
            s.cooldown_blocks = r.u32v();
            s.pitch_jitter_cents_q16 = r.i32v();
            s.gain_jitter_db_q16 = r.i32v();
            const u8 flags = r.u8v();
            s.live = (flags & 1) != 0;
            s.loop = (flags & 2) != 0;
            s.live_authored = (flags & 4) != 0;
            s.atten.min_dist_q16 = r.i32v();
            s.atten.max_dist_q16 = r.i32v();
            s.atten.curve = static_cast<CurveId>(r.u8v());
            s.atten.spread_q16 = r.i32v();
            s.atten.rear_gain_q16 = r.i32v();
            s.atten.air_lpf_hz_q16 = r.i32v();
            s.duck_gain_q16 = r.i32v();
            s.duck_blocks = r.u32v();
            s.variant_first = r.u16v();
            s.variant_count = r.u16v();

            if (s.bus >= out.buses.size()) {
                rep.error("sound '" + s.name + "': bus index out of range");
                return rep;
            }
            if (static_cast<size_t>(s.variant_first) + s.variant_count > out.variants.size()) {
                rep.error("sound '" + s.name + "': variant range out of bounds");
                return rep;
            }
            if (s.atten.curve >= CurveId::COUNT) {
                rep.error("sound '" + s.name + "': unknown attenuation curve");
                return rep;
            }
            out.sounds.push_back(std::move(s));
        }
        if (!r.ok) {
            rep.error("truncated SNDS section");
            return rep;
        }
    }

    // AFTER the sounds are read, not before: this indexes out.sounds, and the
    // SNDS section is what fills it. Read early it silently dropped every
    // mode, because `i < out.sounds.size()` was false for all of them and a
    // bank that rang perfectly as .sfxproj fell silent as .sfxb.
    //
    // Optional. Absent means no sound rings, which is what every bank written
    // before this section existed meant.
    if (const Frame::Ent* reso = f.find(TAG_RESO)) {
        Reader r2 = f.reader(reso);
        const u32 n = r2.u32v();
        for (u32 i = 0; i < n && r2.ok; ++i) {
            const u8 cnt = r2.u8v();
            const i32 dry = r2.i32v();
            ResonatorBank rb;
            rb.dry_q16 = dry;
            for (u8 m = 0; m < cnt && r2.ok; ++m) {
                ResonatorDef d;
                d.hz_q16 = r2.i32v();
                d.decay_ms_q16 = r2.i32v();
                d.gain_q16 = r2.i32v();
                if (m < MAX_RESONATORS) rb.modes[rb.count++] = d;
            }
            if (i < out.sounds.size()) out.sounds[i].resonators = rb;
        }
        if (!r2.ok) {
            rep.error("truncated RESO section");
            return rep;
        }
    }

    if (const Frame::Ent* e = f.find(TAG_FX)) {
        Reader r = f.reader(e);
        read_fx(r, out);
    }
    // MSTR absent means bypass, exactly as it does in a .mstk.
    if (const Frame::Ent* e = f.find(TAG_MSTR)) {
        Reader r = f.reader(e);
        read_master(r, out);
    }
    if (meta) {
        if (const Frame::Ent* e = f.find(TAG_META)) {
            Reader r = f.reader(e);
            read_meta(r, *meta);
        }
    }
    return rep;
}

bool seal_bank(std::vector<u8>& bank, const u8 hash[32], const std::string& script_name) {
    LoadResult rep;
    Frame f;
    if (!open_frame(bank.data(), bank.size(), f, rep)) return false;
    const Frame::Ent* e = f.find(TAG_META);
    if (!e) return false;

    // META is rebuilt rather than patched in place: the script name is a
    // length-prefixed string, so stamping a longer one would move every byte
    // after it. Rebuilding is a few hundred bytes and cannot go subtly wrong.
    BankMeta m;
    Reader r = f.reader(e);
    read_meta(r, m);
    if (!r.ok) return false;

    std::vector<u8> body;
    Writer w{body};
    w.str(m.name);
    w.u32v(m.sound_count);
    w.u32v(m.variant_count);
    w.u32v(m.patch_count);
    w.u8v(1);
    w.blob(hash, 32);
    w.str(script_name);
    w.blob(m.content_hash, 32);

    // Rebuild the payload with the new META, then re-seal.
    std::vector<u8> payload;
    std::vector<Frame::Ent> table;
    for (const auto& old : f.table) {
        const u8* src = f.payload + old.off;
        const size_t n = old.size;
        table.push_back({old.tag, static_cast<u32>(payload.size()), 0});
        if (old.tag == TAG_META) {
            payload.insert(payload.end(), body.begin(), body.end());
            table.back().size = static_cast<u32>(body.size());
        } else {
            payload.insert(payload.end(), src, src + n);
            table.back().size = static_cast<u32>(n);
        }
    }

    std::vector<u8> out;
    Writer hw{out};
    hw.u32v(SFXB_MAGIC);
    hw.u16v(SFXB_VERSION);
    hw.u16v(0);
    const std::string hex = msynth::sha256_hex(payload.data(), payload.size());
    hw.blob(hex.data(), 64);
    hw.u32v(static_cast<u32>(table.size()));
    for (const auto& t : table) {
        hw.u32v(t.tag);
        hw.u32v(t.off);
        hw.u32v(t.size);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    bank.swap(out);
    return true;
}

}  // namespace dsnd
