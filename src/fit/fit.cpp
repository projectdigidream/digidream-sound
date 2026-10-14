#include "fit.h"

#include <algorithm>
#include <complex>
#include <string>
#include <cmath>
#include <random>
#include <cstdio>

namespace dsnd {
namespace fit {

std::vector<Arch> architectures() {
    return {
        // A noise crack, a swept body and a low tail. Weapons, and the most
        // used recipe in any game set.
        {"gunshot",
         R"({"name":"fit_gunshot","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"depth":%g}},
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"fm2op","frameCount":8,
                         "params":{"ratio":3,"startIndex":%g,"endIndex":0.5}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.2}},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.3}}]})",
         {{"crackGain", 0.1f, 1.0f, false},
          {"crackDecay", 5, 120, true},
          {"crackHz", 700, 9000, true},
          {"crackQ", 0.1f, 0.8f, false},
          {"crackFiltDecay", 4, 90, true},
          {"crackFiltDepth", 0, 1.0f, false},
          {"bodyNote", 24, 60, false},
          {"bodyGain", 0.0f, 1.0f, false},
          {"fmIndex", 0.5f, 11, false},
          {"bodyDecay", 20, 400, true},
          {"pitchDecay", 5, 150, true},
          {"pitchDrop", -36, 0, false},
          {"wtDecay", 10, 300, true},
          {"bodyHz", 400, 9000, true},
          {"tailGain", 0.0f, 1.0f, false},
          {"tailDecay", 30, 900, true},
          {"tailHz", 150, 2500, true}},
         0.9f, false, 36, 0.0f},

        // Rockets, barrels, gibs. A transient, a long low body and a folded
        // mid that gives it grit.
        {"explosion",
         R"({"name":"fit_explosion","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":%g,"resonance":0.2}},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"depth":%g}},
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"waveshaper","frameCount":8,
                         "params":{"shape":"fold","input":"triangle",
                                   "startDrive":%g,"endDrive":1,"bias":0.3}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1}}]})",
         {{"crackGain", 0.0f, 1.0f, false}, {"crackDecay", 4, 90, true},
          {"crackHz", 1200, 9000, true},
          {"bodyGain", 0.2f, 1.0f, false}, {"bodyDecay", 100, 1600, true},
          {"bodyHz", 150, 2500, true}, {"bodyQ", 0.1f, 0.8f, false},
          {"bodyFiltDecay", 40, 900, true}, {"bodyFiltDepth", 0, 1.0f, false},
          {"gritNote", 20, 52, false}, {"gritGain", 0.0f, 1.0f, false},
          {"gritDrive", 1, 16, false}, {"gritDecay", 40, 700, true},
          {"gritPitchDecay", 10, 300, true}, {"gritDrop", -36, 0, false},
          {"gritWtDecay", 20, 500, true}},
         1.6f, false, 36, 0.0f},

        // A weapon with a MECHANICAL ACTION: the shot, and then the gun.
        //
        // The nine families above all fire everything at t=0 and decay, which
        // is right for a hit and wrong for a firearm. A super shotgun is a
        // sequence -- crack, body, the barrel breaking open, shells rattling --
        // spread over most of a second, and no amount of optimiser effort can
        // place an event at 300 ms if every layer starts at zero.
        //
        // So this one spends two of its four layers on DELAYED events, and
        // lets the optimiser choose when they happen. startDelayMs has been in
        // the engine all along (patch_def.h: "layer speaks this many control
        // blocks late"); it had simply never been offered to the fitter.
        {"weapon",
         R"({"name":"fit_weapon","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":%g,"resonance":0.2}},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"depth":%g}},
           {"osc":"noise","noiseColor":"white","mixGain":%g,"startDelayMs":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g}},
           {"osc":"noise","noiseColor":"pink","mixGain":%g,"startDelayMs":%g,
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g}}]})",
         {{"crackGain", 0.0f, 1.0f, false}, {"crackDecay", 3, 70, true},
          {"crackHz", 1500, 11000, true},
          {"bodyGain", 0.2f, 1.0f, false}, {"bodyDecay", 40, 900, true},
          {"bodyHz", 120, 2500, true}, {"bodyQ", 0.1f, 0.8f, false},
          {"bodyFiltDecay", 20, 600, true}, {"bodyFiltDepth", 0, 1.0f, false},
          {"mechGain", 0.0f, 0.9f, false}, {"mechAt", 20, 500, true},
          {"mechDecay", 5, 200, true}, {"mechHz", 600, 8000, true},
          {"mechQ", 0.2f, 0.9f, false},
          {"tailGain", 0.0f, 0.8f, false}, {"tailAt", 80, 900, true},
          {"tailAttack", 0, 120, false}, {"tailDecay", 20, 500, true},
          {"tailHz", 400, 6000, true}, {"tailQ", 0.1f, 0.8f, false}},
         1.2f, false, 36, 0.0f},

        // MODAL: a short exciter ringing a body.
        //
        // The other architectures build a sound out of noise and filters,
        // which is subtractive synthesis and is the wrong shape for anything
        // STRUCK. A hit on metal, wood or a casing is a brief broadband
        // excitation followed by a handful of exponentially decaying
        // sinusoids at the object's own resonant frequencies -- that is what
        // modal synthesis means, and it is what the literature reaches for
        // first for impacts.
        //
        // So here the patch is only the exciter, and four modes carry the
        // identity of the thing being hit. `resonators` and `resonatorDry`
        // are sound-level fields; SfxRenderer hoists them out of the patch.
        {"modal",
         R"({"name":"fit_modal","oneShot":true,
           "resonatorDry":%g,
           "resonators":[{"hz":%g,"decayMs":%g,"gain":%g},
                         {"hz":%g,"decayMs":%g,"gain":%g},
                         {"hz":%g,"decayMs":%g,"gain":%g},
                         {"hz":%g,"decayMs":%g,"gain":%g}],
           "layers":[
           {"osc":"noise","noiseColor":"%s","mixGain":1,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.2}}]})",
         {{"dry", 0.0f, 1.2f, false},
          {"m1Hz", 40, 1200, true}, {"m1Decay", 20, 2000, true}, {"m1Gain", 0.0f, 2.0f, false},
          {"m2Hz", 80, 3000, true}, {"m2Decay", 15, 1200, true}, {"m2Gain", 0.0f, 1.6f, false},
          {"m3Hz", 200, 6000, true}, {"m3Decay", 10, 700, true}, {"m3Gain", 0.0f, 1.2f, false},
          {"m4Hz", 400, 11000, true}, {"m4Decay", 5, 400, true}, {"m4Gain", 0.0f, 1.0f, false},
          {"exciteDecay", 1, 60, true}, {"exciteHz", 500, 12000, true}},
         1.4f, false, 36, 0.0f},

        // ROAR: sounds that SWELL or HOLD, rather than hit and decay.
        //
        // Every other architecture here starts at full level and fades -- an
        // attack of zero, or at most forty milliseconds. That is right for a
        // hit and wrong for a large share of any game's set: a teleport that
        // swells to its peak two-thirds of the way through, a BFG that holds
        // near full scale for a second and a half, a revenant missile that
        // rises and sustains, a fireball whose roar undulates. Measured, those
        // were the worst-fitting roles in the DOOM set, and they had only ever
        // been offered tonal families that cannot make them.
        //
        // The engine could always do it: a one-shot envelope runs attack ->
        // decay, so a long ATTACK is a swell and a long DECAY after a short
        // attack is a hold. This family finally lets the optimiser choose both,
        // on a swept noise body with an LFO wobbling its filter and level, and
        // a tonal FM buzz with its own swell underneath. The LFO's rate is
        // named `lfoRate`, not "...Hz", on purpose: peak seeding would
        // otherwise start it on an audio-frequency spectral peak.
        {"roar",
         R"({"name":"fit_roar","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"%s","mixGain":%g,
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":%g,"decay":%g,"sustain":0,"depth":%g},
            "lfos":[{"shape":"sine","rateHz":%g}],
            "modMatrix":[{"src":"LFO1","dst":"FilterCutoff","amount":%g},
                         {"src":"LFO1","dst":"Amplitude","amount":%g}]},
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"fm2op","frameCount":8,
                         "params":{"ratio":2,"startIndex":%g,"endIndex":0.5}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.2}}]})",
         {{"noiseGain", 0.2f, 1.0f, false}, {"swell", 1, 900, true}, {"hold", 60, 2500, true},
          {"noiseHz", 200, 8000, true}, {"noiseQ", 0.1f, 0.85f, false},
          {"sweepAttack", 1, 900, true}, {"sweepDecay", 30, 2000, true},
          {"sweepDepth", 0.0f, 1.0f, false},
          {"lfoRate", 0.5f, 14.0f, true}, {"lfoToFilter", 0.0f, 0.5f, false},
          {"lfoToLevel", 0.0f, 0.6f, false},
          {"buzzNote", 24, 72, false}, {"buzzGain", 0.0f, 1.0f, false},
          {"buzzIndex", 0.5f, 10.0f, false}, {"buzzSwell", 1, 900, true},
          {"buzzHold", 60, 2500, true}, {"buzzPitchDecay", 20, 1500, true},
          {"buzzDrop", -24, 24, false}, {"buzzWtDecay", 20, 1500, true},
          {"buzzCutHz", 500, 10000, true}},
         1.8f, false, 36, 0.0f},

        // GROWL: a voice that HOLDS.
        //
        // `voice` is one-shot, and a one-shot envelope has its sustain forced
        // to zero -- it can only approximate a plateau with a slow fall. That
        // was most of `voice`'s error on DOOM: the worst monster references
        // sit near full scale for one to two seconds (arachnotron death is at
        // 99% in all ten tenths of its length), and nothing could hold.
        //
        // This is the same throat with a real sustain, released by a gesture
        // note of length `holdMs` (hoisted by SfxRenderer -- see there), plus a
        // swell, and an LFO driving level and pitch: at tens of hertz that is
        // what makes a voice a GROWL rather than a tone. The LFO's rate is
        // `growlRate`, not "...Hz", so peak seeding leaves it alone.
        {"growl",
         R"({"name":"fit_growl","oneShot":false,"holdMs":%g,"layers":[
           {"pitchNote":%g,"mixGain":1,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"formant","cutoffHz":%g,"resonance":%g,
                      "vowel":"a","endVowel":"o"},
            "filterEnv":{"attack":%g,"decay":%g,"sustain":0,"depth":%g},
            "postDrive":%g,
            "lfos":[{"shape":"sine","rateHz":%g}],
            "modMatrix":[{"src":"LFO1","dst":"Amplitude","amount":%g},
                         {"src":"LFO1","dst":"Pitch","amount":%g}]},
           {"osc":"noise","noiseColor":"pink","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":%g,"release":%g,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":0.4}}]})",
         {{"holdMs", 120, 2000, true},
          {"note", 24, 72, false},
          {"startHarm", 4, 64, false}, {"endHarm", 1, 32, false},
          {"swell", 1, 600, true}, {"decay", 40, 1200, true},
          {"sustain", 0.15f, 1.0f, false}, {"release", 30, 900, true},
          {"pitchDecay", 20, 1500, true}, {"pitchDrop", -18, 6, false},
          {"wtDecay", 40, 2000, true},
          {"formantHz", 260, 1600, true}, {"formantQ", 0.3f, 0.9f, false},
          {"filtAttack", 0, 400, false}, {"filtDecay", 40, 1500, true},
          {"filtDepth", 0, 1.0f, false},
          {"drive", 1, 6, false},
          {"growlRate", 3.0f, 40.0f, true}, {"growlDepth", 0.0f, 0.7f, false},
          {"wobble", 0.0f, 0.02f, false},
          {"breathGain", 0.0f, 0.9f, false}, {"breathDecay", 40, 1200, true},
          {"breathSustain", 0.0f, 0.8f, false}, {"breathRelease", 30, 900, true},
          {"breathHz", 500, 5000, true}},
         2.0f, false, 36, 0.0f},

        // BURST: several wet events, not one.
        //
        // A gib splat is ten onsets in a second; a claw swipe or a bite is
        // five. Every older family fires each layer once, at most four of
        // them, so a sound made of many small events could only ever be
        // smeared into one. The engine has always had the answer -- a
        // variant's gesture re-triggers its patch, up to eight times -- and no
        // family used it. This one is a short splat plus a low thump, and an
        // eight-step gesture whose times and velocities are fitted. The times
        // are `...At`, so they start on the reference's detected onsets:
        // descent cannot move an event through time any more than it can move
        // a mode through frequency.
        {"burst",
         R"({"name":"fit_burst","oneShot":true,
           "gesture":[{"atMs":0,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g},
                      {"atMs":%g,"note":60,"vel":%g}],
           "layers":[
           {"osc":"noise","noiseColor":"%s","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"depth":%g}},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.3}}]})",
         {{"v1", 1, 127, false},
          {"t2At", 10, 1600, true}, {"v2", 1, 127, false},
          {"t3At", 10, 1600, true}, {"v3", 1, 127, false},
          {"t4At", 10, 1600, true}, {"v4", 1, 127, false},
          {"t5At", 10, 1600, true}, {"v5", 1, 127, false},
          {"t6At", 10, 1600, true}, {"v6", 1, 127, false},
          {"t7At", 10, 1600, true}, {"v7", 1, 127, false},
          {"t8At", 10, 1600, true}, {"v8", 1, 127, false},
          {"splatGain", 0.1f, 1.0f, false}, {"splatDecay", 5, 250, true},
          {"splatHz", 300, 6000, true}, {"splatQ", 0.1f, 0.85f, false},
          {"sweepDecay", 5, 300, true}, {"sweepDepth", 0.0f, 1.0f, false},
          {"thumpGain", 0.0f, 1.0f, false}, {"thumpDecay", 10, 400, true},
          {"thumpHz", 60, 1200, true}},
         1.6f, false, 36, 0.0f},

        // SEQUENCE: a few pitched events -- a pickup is two tones 160 ms
        // apart, a switch two clicks. The layers carry NO pitchNote, so each
        // gesture step's note is the pitch it plays at; four steps, with
        // times, notes and velocities fitted.
        {"sequence",
         R"({"name":"fit_sequence","oneShot":true,
           "gesture":[{"atMs":0,"note":%g,"vel":%g},
                      {"atMs":%g,"note":%g,"vel":%g},
                      {"atMs":%g,"note":%g,"vel":%g},
                      {"atMs":%g,"note":%g,"vel":%g}],
           "layers":[
           {"mixGain":1,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"%s"}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.2}},
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":2500,"resonance":0.2}}]})",
         {{"n1", 36, 96, false}, {"v1", 1, 127, false},
          {"t2At", 10, 900, true}, {"n2", 36, 96, false}, {"v2", 1, 127, false},
          {"t3At", 10, 900, true}, {"n3", 36, 96, false}, {"v3", 1, 127, false},
          {"t4At", 10, 900, true}, {"n4", 36, 96, false}, {"v4", 1, 127, false},
          {"startHarm", 1, 32, false}, {"endHarm", 1, 16, false},
          {"toneDecay", 20, 400, true}, {"toneHz", 800, 10000, true},
          {"clickGain", 0.0f, 0.8f, false}, {"clickDecay", 1, 30, true}},
         0.9f, false, 36, 0.0f},

        // MOTOR: a sustained, driven, PITCHED source that pulses.
        //
        // A chainsaw is strongly periodic (0.56 autocorrelation at 170 Hz)
        // and holds for over a second; its idle is an engine firing at ~54 Hz;
        // a lift door is a pitched grind. `mechanism` is a noise thunk and a
        // slide -- the wrong kind of sound, not the wrong numbers. This is a
        // harmonic source through drive and a low-pass, held for `holdMs`,
        // with an LFO on level (the firing) and pitch (the wobble) that can run
        // up to 90 Hz, a spin-up pitch envelope, and a noise bed underneath.
        // `motorRate` is not "...Hz", so peak seeding leaves it alone.
        {"motor",
         R"({"name":"fit_motor","oneShot":false,"holdMs":%g,"layers":[
           {"pitchNote":%g,"mixGain":1,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g},
            "postDrive":%g,
            "lfos":[{"shape":"sine","rateHz":%g}],
            "modMatrix":[{"src":"LFO1","dst":"Amplitude","amount":%g},
                         {"src":"LFO1","dst":"Pitch","amount":%g}]},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":%g,"release":%g,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.3}}]})",
         {{"holdMs", 80, 2000, true}, {"note", 24, 84, false},
          {"startHarm", 4, 64, false}, {"endHarm", 2, 48, false},
          {"attack", 1, 300, true}, {"decay", 20, 1200, true},
          {"sustain", 0.2f, 1.0f, false}, {"release", 20, 600, true},
          {"spinDecay", 20, 1500, true}, {"spinDrop", -24, 12, false},
          {"motorHz", 300, 8000, true}, {"motorQ", 0.1f, 0.8f, false},
          {"drive", 1, 6, false},
          {"motorRate", 2.0f, 90.0f, true}, {"pulse", 0.0f, 0.8f, false},
          {"wobble", 0.0f, 0.02f, false},
          {"bedGain", 0.0f, 0.8f, false}, {"bedDecay", 20, 1200, true},
          {"bedSustain", 0.0f, 0.8f, false}, {"bedRelease", 20, 600, true},
          {"bedHz", 100, 4000, true}},
         1.8f, false, 36, 0.0f},

        // Plasma, BFG, teleports. Bright FM whose index falls.
        {"energy",
         R"({"name":"fit_energy","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":1,
            "wavetable":{"generator":"fm2op","frameCount":8,
                         "params":{"ratio":%g,"startIndex":%g,"endIndex":%g}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g}},
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":0.5}}]})",
         {{"note", 30, 84, false}, {"ratio", 1, 15, false},
          {"startIndex", 1, 12, false}, {"endIndex", 0, 4, false},
          {"attack", 0, 40, false}, {"decay", 40, 1400, true},
          {"pitchDecay", 10, 600, true}, {"pitchDrop", -24, 24, false},
          {"wtDecay", 20, 1000, true}, {"cutoff", 600, 12000, true},
          {"reso", 0, 0.7f, false},
          {"airGain", 0.0f, 0.8f, false}, {"airDecay", 10, 300, true},
          {"airHz", 1500, 11000, true}},
         1.4f, false, 36, 0.0f},

        // Flesh, wall, metal, footsteps. Noise shaped by a filter plus a
        // short saturated body.
        {"impact",
         R"({"name":"fit_impact","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"%s","mixGain":1,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"%s","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"depth":%g}},
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"waveshaper","frameCount":8,
                         "params":{"shape":"soft","input":"sine",
                                   "startDrive":%g,"endDrive":1}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1}}]})",
         {{"decay", 15, 400, true}, {"cutoff", 250, 9000, true},
          {"reso", 0.1f, 0.8f, false}, {"filtDecay", 8, 300, true},
          {"filtDepth", 0, 1.0f, false},
          {"bodyNote", 24, 64, false}, {"bodyGain", 0.0f, 1.0f, false},
          {"drive", 1, 8, false}, {"bodyDecay", 10, 300, true},
          {"pitchDecay", 5, 150, true}, {"pitchDrop", -30, 0, false},
          {"wtDecay", 10, 250, true}},
         0.7f, false, 36, 0.0f},

        // Monsters. The formant filter morphed by the filter envelope is a
        // growl machine, and `cutoffHz` rescales all the formants at once --
        // one knob for the size of the creature. This is the architecture
        // that makes forty monster sounds tractable.
        {"voice",
         R"({"name":"fit_voice","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":1,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"formant","cutoffHz":%g,"resonance":%g,
                      "vowel":"a","endVowel":"o"},
            "filterEnv":{"attack":%g,"decay":%g,"sustain":0,"depth":%g},
            "postDrive":%g},
           {"osc":"noise","noiseColor":"pink","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":0.4}}]})",
         {{"note", 24, 72, false},
          {"startHarm", 4, 64, false}, {"endHarm", 1, 32, false},
          {"attack", 0, 120, false}, {"decay", 80, 1600, true},
          {"pitchDecay", 20, 900, true}, {"pitchDrop", -18, 6, false},
          {"wtDecay", 40, 1200, true},
          {"formantHz", 260, 1600, true}, {"formantQ", 0.3f, 0.9f, false},
          {"filtAttack", 0, 200, false}, {"filtDecay", 40, 900, true},
          {"filtDepth", 0, 1.0f, false},
          {"drive", 1, 6, false},
          {"breathGain", 0.0f, 0.9f, false}, {"breathDecay", 40, 1200, true},
          {"breathHz", 500, 5000, true}},
         1.6f, false, 36, 0.0f},

        // Chainsaw, lifts, platforms: a driven saw plus a rumble bed.
        {"machine",
         R"({"name":"fit_machine","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"waveshaper","frameCount":8,
                         "params":{"shape":"hard","input":"saw",
                                   "startDrive":%g,"endDrive":%g}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g}},
           {"osc":"noise","noiseColor":"brown","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":0.3}}]})",
         {{"note", 20, 60, false}, {"gain", 0.2f, 1.0f, false},
          {"startDrive", 1, 12, false}, {"endDrive", 1, 12, false},
          {"attack", 0, 120, false}, {"decay", 60, 1400, true},
          {"wtDecay", 30, 900, true},
          {"cutoff", 200, 6000, true}, {"reso", 0, 0.7f, false},
          {"bedGain", 0.0f, 0.9f, false}, {"bedDecay", 40, 1200, true},
          {"bedHz", 150, 2000, true}},
         1.4f, false, 36, 0.0f},

        // Doors, switches, racks: a thunk and a filtered slide.
        {"mechanism",
         R"({"name":"fit_mechanism","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"brown","mixGain":1,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g}},
           {"mixGain":%g,"startDelayMs":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"flat"}},
            "pitchNote":%g,
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":%g,"decay":0,"sustain":1,"depthSemis":%g},
            "wtPosEnv":{"attack":%g,"decay":0,"sustain":1,"depth":1},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g}}]})",
         {{"thunkDecay", 20, 500, true}, {"thunkHz", 150, 2000, true},
          {"thunkQ", 0.1f, 0.7f, false},
          {"slideGain", 0.0f, 0.9f, false}, {"slideDelay", 0, 400, false},
          {"startHarm", 1, 32, false}, {"endHarm", 1, 48, false},
          {"slideNote", 30, 72, false},
          {"slideAttack", 0, 300, false}, {"slideDecay", 80, 1600, true},
          {"slidePitchAttack", 20, 900, true}, {"slidePitchSemis", -12, 12, false},
          {"slideWtAttack", 20, 900, true},
          {"slideHz", 300, 6000, true}, {"slideQ", 0.2f, 0.9f, false}},
         1.6f, false, 36, 0.0f},

        // Pickups, menus, the small bright ones.
        {"blip",
         R"({"name":"fit_blip","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":1,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"%s"}},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"depthSemis":%g},
            "wtPosEnv":{"attack":0,"decay":%g,"sustain":0,"depth":1},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g}}]})",
         {{"note", 48, 100, false},
          {"startHarm", 1, 24, false}, {"endHarm", 1, 16, false},
          {"attack", 0, 30, false}, {"decay", 20, 600, true},
          {"pitchDecay", 5, 300, true}, {"pitchDrop", -24, 24, false},
          {"wtDecay", 10, 400, true},
          {"cutoff", 1000, 12000, true}, {"reso", 0, 0.6f, false}},
         0.8f, false, 36, 0.0f},

        // Swings, jumps, passing things.
        {"whoosh",
         R"({"name":"fit_whoosh","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"pink","mixGain":1,
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g},
            "filterEnv":{"attack":%g,"decay":%g,"sustain":0,"depth":%g}}]})",
         {{"attack", 0, 200, false}, {"decay", 40, 900, true},
          {"cutoff", 300, 8000, true}, {"reso", 0.2f, 0.9f, false},
          {"filtAttack", 0, 200, false}, {"filtDecay", 20, 600, true},
          {"filtDepth", 0, 1.0f, false}},
         0.9f, false, 36, 0.0f},
    };
}

// A loss that keeps the target's analysis.
//
// midisynth's spectral_loss analyses BOTH sides on every call, which is right
// for a one-shot comparison and ruinous inside a descent: the target never
// changes, and its three spectrograms plus its band profile were being
// recomputed for every one of the thousands of candidates. Measured, the loss
// was 96 ms against 0.9 ms for the render it was judging -- ninety-nine per
// cent of a fit, spent re-analysing a constant.
//
// The arithmetic below is midisynth's, term for term; only the target side is
// remembered.
// The loss, measured only where the reference has information.
//
// Three terms, as before -- a multi-resolution log spectrogram, an amplitude
// envelope in dB, and an octave-band balance -- with three changes that are
// the whole point of this class:
//
//   1. BAND-LIMITED. Every term is computed over bins at or below `band_hz`,
//      the reference's own Nyquist. A DOOM lump is 11 kHz; above 5.5 kHz it
//      holds nothing but the resampler's images and 8-bit hiss, and those bins
//      were about 80% of the distance the fitter used to minimise. What a
//      sound does above the band is now simply not scored -- which is a
//      decision, not an accident: the user chose to leave a fitted sound FREE
//      there rather than hold it dark.
//
//   2. THE ENVELOPE IS BAND-LIMITED TOO. It used to be full-band RMS, so a
//      candidate's unscored high end would still have leaked into the score
//      through its loudness. It is now the in-band energy of each 1024-point
//      frame -- a hop of 5.3 ms, close to the old 5 ms blocks.
//
//   3. A NOISE FLOOR, 60 dB below the target's loudest bin (and, for the
//      envelope, its loudest frame), applied to BOTH signals. Below it an
//      8-bit reference is hiss, and a candidate that decays to true silence
//      under a reference that decays into hiss is not wrong.
//
// It also replaces two implementations with one. The fitter used a cached
// copy of this arithmetic and the scorer used midisynth's spectral_loss; they
// agreed only because nobody had changed either. score() below is the same
// class, so the fitter, dsound_score, keep_better.py and the browser now use
// literally one measurement.
class BandLoss {
  public:
    void set_target(const std::vector<float>& t, const LossWeights& w, float band_hz) {
        w_ = w;
        band_ = band_hz > 0 ? band_hz : 0.5f * SAMPLE_RATE;
        for (int r = 0; r < 3; ++r) {
            analyse(t, kSizes[r], kmax(r), tmag_[r], tframes_[r]);
            float peak = 0;
            for (float v : tmag_[r]) peak = std::max(peak, v);
            floor_[r] = std::max(1e-5f, peak * kFloor);
            tlog_[r].resize(tmag_[r].size());
            for (size_t i = 0; i < tmag_[r].size(); ++i)
                tlog_[r][i] = std::log(std::max(tmag_[r][i], floor_[r]));
        }
        tenv_ = envelope(tmag_[1], tframes_[1], kmax(1));
        float epeak = -300;
        for (float v : tenv_) epeak = std::max(epeak, v);
        env_floor_ = epeak - kFloorDb;
        for (float& v : tenv_) v = std::max(v, env_floor_);
        tband_ = bands(tmag_[2], tframes_[2], kmax(2));
    }

    float operator()(const std::vector<float>& b) const {
        std::vector<float> mag[3];
        size_t frames[3] = {0, 0, 0};
        for (int r = 0; r < 3; ++r) analyse(b, kSizes[r], kmax(r), mag[r], frames[r]);

        float stft = 0;
        int terms = 0;
        if (w_.stft > 0) {
            for (int r = 0; r < 3; ++r) {
                const size_t n = std::min(tlog_[r].size(), mag[r].size());
                if (!n) continue;
                float acc = 0;
                for (size_t i = 0; i < n; ++i)
                    acc += std::fabs(tlog_[r][i] - std::log(std::max(mag[r][i], floor_[r])));
                stft += acc / static_cast<float>(n);
                ++terms;
            }
            stft = terms ? stft / static_cast<float>(terms) : 0;
        }

        float env = 0;
        if (w_.env > 0) {
            auto eb = envelope(mag[1], frames[1], kmax(1));
            const size_t n = std::min(tenv_.size(), eb.size());
            for (size_t i = 0; i < n; ++i) env += std::fabs(tenv_[i] - std::max(eb[i], env_floor_));
            env = n ? env / static_cast<float>(n) : 0;
        }

        float bnd = 0;
        if (w_.bands > 0) {
            const auto pb = bands(mag[2], frames[2], kmax(2));
            for (size_t i = 0; i < tband_.size() && i < pb.size(); ++i)
                bnd += std::fabs(tband_[i] - pb[i]);
        }
        return w_.stft * stft + w_.env * env + w_.bands * bnd;
    }

  private:
    static constexpr size_t kSizes[3] = {256, 1024, 4096};
    static constexpr float kFloorDb = 60.0f;
    static constexpr float kFloor = 0.001f;   // 10^(-60/20)

    // Bins at or below the band, per resolution.
    size_t kmax(int r) const {
        const size_t N = kSizes[r];
        size_t k = static_cast<size_t>(band_ * static_cast<float>(N) / SAMPLE_RATE) + 1;
        return std::min(k, N / 2);
    }

    // Hann-windowed frames at hop N/4 -- the same framing log_spectrum uses --
    // keeping only the in-band magnitudes, frame-major.
    static void analyse(const std::vector<float>& x, size_t N, size_t kmax,
                        std::vector<float>& mag, size_t& frames) {
        mag.clear();
        frames = 0;
        if (x.size() < N) return;
        std::vector<float> win(N);
        for (size_t i = 0; i < N; ++i)
            win[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / N);
        for (size_t off = 0; off + N <= x.size(); off += N / 4) {
            std::vector<std::complex<float>> buf(N);
            for (size_t i = 0; i < N; ++i) buf[i] = {x[off + i] * win[i], 0.0f};
            msynth::fit::fft(buf);
            for (size_t k = 0; k < kmax; ++k) mag.push_back(std::abs(buf[k]));
            ++frames;
        }
    }

    // In-band energy per frame, in dB.
    static std::vector<float> envelope(const std::vector<float>& mag, size_t frames, size_t kmax) {
        std::vector<float> out(frames);
        for (size_t f = 0; f < frames; ++f) {
            float e = 0;
            for (size_t k = 0; k < kmax; ++k) {
                const float m = mag[f * kmax + k];
                e += m * m;
            }
            out[f] = 10.0f * std::log10(1e-12f + e);
        }
        return out;
    }

    // The octave-band balance, as a percentage of IN-BAND energy, over the
    // bands that start below the reference's Nyquist.
    std::vector<float> bands(const std::vector<float>& mag, size_t frames, size_t kmax) const {
        static const float EDGES[] = {0, 35, 60, 120, 250, 600, 1500, 4000, 24000};
        constexpr int NB = 8;
        std::vector<float> acc(NB, 0.0f);
        const float N = static_cast<float>(kSizes[2]);
        for (size_t f = 0; f < frames; ++f)
            for (size_t k = 0; k < kmax; ++k) {
                const float hz = static_cast<float>(k) * SAMPLE_RATE / N;
                const float m = mag[f * kmax + k];
                for (int b = 0; b < NB; ++b)
                    if (hz >= EDGES[b] && hz < EDGES[b + 1]) { acc[b] += m * m; break; }
            }
        int live = 0;
        while (live < NB && EDGES[live] < band_) ++live;
        acc.resize(static_cast<size_t>(live));
        float tot = 0;
        for (float v : acc) tot += v;
        if (tot <= 0) tot = 1;
        for (float& v : acc) v = 100.0f * v / tot;
        return acc;
    }

    LossWeights w_;
    float band_ = 0.5f * SAMPLE_RATE;
    std::vector<float> tmag_[3], tlog_[3];
    size_t tframes_[3] = {0, 0, 0};
    float floor_[3] = {1e-5f, 1e-5f, 1e-5f};
    std::vector<float> tenv_;
    float env_floor_ = -300;
    std::vector<float> tband_;
};

constexpr size_t BandLoss::kSizes[3];
constexpr float BandLoss::kFloorDb;
constexpr float BandLoss::kFloor;

float score(const std::vector<float>& target, const std::vector<float>& candidate,
            const LossWeights& w, float band_hz) {
    BandLoss l;
    l.set_target(target, w, band_hz);
    return l(candidate);
}

namespace {

// Two architectures carry a categorical choice the optimiser cannot cross --
// a noise colour, a filter mode, a falloff law. They are enumerated instead:
// each combination is fitted as its own variant of the architecture, which is
// the same reason the structural choices are fixed by hand in the first place.
struct Categorical {
    const char* arch;
    std::vector<std::vector<const char*>> options;
};

// One expansion: the suffix that names the choices, and the filled template.
struct Expansion {
    std::string suffix;  // "" for an architecture with no categoricals
    std::string json;
};

std::vector<Expansion> expand_categoricals(const Arch& a) {
    std::string tpl = a.json;
    std::vector<std::vector<const char*>> sets;
    if (std::string(a.name) == "impact")
        sets = {{"white", "pink", "brown"}, {"lowpass", "bandpass", "highpass"}};
    else if (std::string(a.name) == "blip")
        sets = {{"saw", "square", "triangle", "flat"}};
    else if (std::string(a.name) == "modal")
        sets = {{"white", "pink", "brown"}};
    else if (std::string(a.name) == "roar")
        sets = {{"pink", "white"}};
    else if (std::string(a.name) == "burst")
        sets = {{"pink", "white"}};
    else if (std::string(a.name) == "sequence")
        sets = {{"saw", "square"}};
    if (sets.empty()) return {{"", tpl}};

    std::vector<Expansion> out{{"", tpl}};
    for (const auto& set : sets) {
        std::vector<Expansion> next;
        for (const auto& e : out) {
            const size_t hit = e.json.find("%s");
            if (hit == std::string::npos) {
                next.push_back(e);
                continue;
            }
            for (const char* opt : set) {
                // "/" before the first choice, "+" between the rest, so an
                // expanded name reads as impact/brown+highpass.
                next.push_back({e.suffix + (e.suffix.empty() ? "/" : "+") + opt,
                                e.json.substr(0, hit) + opt + e.json.substr(hit + 2)});
            }
        }
        out.swap(next);
    }
    return out;
}

}  // namespace

std::vector<ExpandedArch> architectures_expanded() {
    // A function-local static, because ExpandedArch::base is a borrowed
    // pointer and architectures() returns by value: pointing into a temporary
    // would dangle. Arch holds only string literals and a small vector, so one
    // immutable copy for the process is both safe and cheap.
    static const std::vector<Arch> base = architectures();

    std::vector<ExpandedArch> out;
    for (const Arch& a : base)
        for (const Expansion& e : expand_categoricals(a))
            out.push_back({std::string(a.name) + e.suffix, e.json, &a});
    return out;
}

Arch instantiate(const ExpandedArch& e) {
    Arch a = *e.base;
    a.json = e.json.c_str();  // borrowed: valid only while `e` lives
    return a;
}

std::vector<float> spectral_peaks(const std::vector<float>& target, size_t want, float band_hz) {
    std::vector<float> out;
    if (want == 0 || target.size() < 2048) return out;

    // One spectrum over the whole window rather than a spectrogram: a mode
    // rings through the sound, so summing across time is what makes it stand
    // out from the transient.
    const size_t N = 4096;
    std::vector<float> mag(N / 2, 0.0f);
    size_t frames = 0;
    for (size_t off = 0; off + N <= target.size(); off += N / 2) {
        std::vector<std::complex<float>> buf(N);
        for (size_t i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / N);
            buf[i] = {target[off + i] * w, 0.0f};
        }
        msynth::fit::fft(buf);
        for (size_t k = 0; k < N / 2; ++k) mag[k] += std::abs(buf[k]);
        ++frames;
    }
    if (frames == 0) return out;

    // Local maxima, sorted by height. A peak must beat both neighbours and sit
    // above the mean, or noise floor ripple supplies as many "peaks" as asked.
    float mean = 0;
    for (float v : mag) mean += v;
    mean /= static_cast<float>(mag.size());

    // Only below the reference's band: above it a resampled reference has
    // nothing real, and a peak there would seed a frequency on an artefact.
    const size_t kband = std::min(mag.size(), static_cast<size_t>(band_hz * N / SAMPLE_RATE) + 1);
    std::vector<std::pair<float, size_t>> peaks;
    for (size_t k = 2; k + 2 < kband; ++k) {
        if (mag[k] <= mean) continue;
        if (mag[k] > mag[k - 1] && mag[k] > mag[k + 1] &&
            mag[k] >= mag[k - 2] && mag[k] >= mag[k + 2]) {
            peaks.push_back({mag[k], k});
        }
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if (peaks.size() > want) peaks.resize(want);

    // Ascending in frequency, because the architectures name their modes in
    // that order (m1Hz below m2Hz) and their ranges assume it.
    std::sort(peaks.begin(), peaks.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& p : peaks)
        out.push_back(static_cast<float>(p.second) * SAMPLE_RATE / static_cast<float>(N));
    return out;
}

std::vector<float> onset_times_ms(const std::vector<float>& target, size_t want) {
    std::vector<float> out;
    if (want == 0 || target.size() < 2048) return out;
    // A 5 ms RMS envelope in dB, and an onset wherever it climbs more than
    // 9 dB over 15 ms within 30 dB of the loudest point -- loud enough to be
    // an event, sharp enough to be an attack rather than a swell.
    const size_t hop = SAMPLE_RATE / 200;
    std::vector<float> db;
    for (size_t off = 0; off + hop <= target.size(); off += hop) {
        float r = 0;
        for (size_t i = 0; i < hop; ++i) r += target[off + i] * target[off + i];
        db.push_back(20.0f * std::log10(1e-6f + std::sqrt(r / static_cast<float>(hop))));
    }
    float top = -200;
    for (float v : db) top = std::max(top, v);
    std::vector<std::pair<float, size_t>> cand;   // (strength, frame)
    size_t last = 0;
    for (size_t i = 3; i < db.size(); ++i) {
        const float rise = db[i] - db[i - 3];
        if (rise > 9.0f && db[i] > top - 30.0f && (cand.empty() || i - last > 8)) {
            // Walk to the local peak of this rise, so the time is the event's
            // arrival rather than the first frame that noticed it.
            size_t j = i;
            while (j + 1 < db.size() && db[j + 1] > db[j]) ++j;
            // Frame i is the first one the event is loud in, and frame i
            // covers [5i, 5i + 5) ms. Recording the start of the 15 ms rise
            // window instead put every onset one window early -- consistently
            // 15 ms, which is enough to seed every event visibly early.
            cand.push_back({rise, i});
            last = i;
            i = j;
        }
    }
    std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& c : cand) {
        const float ms = static_cast<float>(c.second) * 5.0f;
        if (ms < 20.0f) continue;   // the one at the start is not a later event
        out.push_back(ms);
        if (out.size() >= want) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<float> peak_seeded_start(const Arch& arch, const std::vector<float>& target,
                                     float band_hz) {
    const size_t P = arch.params.size();
    std::vector<float> u(P, 0.5f);

    auto ends = [](const char* nm, const char* suf) {
        const std::string n(nm), x(suf);
        return n.size() > x.size() && n.compare(n.size() - x.size(), x.size(), x) == 0;
    };
    size_t wanted = 0, times = 0;
    for (const auto& p : arch.params) {
        if (ends(p.name, "Hz")) ++wanted;
        if (ends(p.name, "At")) ++times;
    }
    // Empty means "nothing to seed", and fit_best then skips the extra
    // restart rather than re-running restart 0 from the same centre -- which
    // is deterministic, so it would only reproduce restart 0 at full cost.
    if (wanted == 0 && times == 0) return {};

    // Map a real value into [0,1] the way denorm() maps out of it, clamped: a
    // peak or onset outside a parameter's range is still a better start at the
    // nearest reachable value than the middle of the range.
    auto norm = [](const Param& p, float v) {
        float t;
        if (p.log) {
            const float lo = std::log(std::max(1e-6f, p.lo));
            const float hi = std::log(std::max(1e-6f, p.hi));
            t = (std::log(std::max(1e-6f, v)) - lo) / (hi - lo);
        } else {
            t = (v - p.lo) / (p.hi - p.lo);
        }
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    };

    const std::vector<float> onsets = times ? onset_times_ms(target, times) : std::vector<float>();
    size_t next_t = 0;
    for (size_t i = 0; i < P && !onsets.empty(); ++i) {
        if (!ends(arch.params[i].name, "At")) continue;
        if (next_t >= onsets.size()) break;   // fewer events than slots: leave the rest centred
        u[i] = norm(arch.params[i], onsets[next_t++]);
    }

    const bool timed = next_t > 0;
    if (wanted == 0) return timed ? u : std::vector<float>();
    const std::vector<float> peaks = spectral_peaks(target, wanted, band_hz);
    if (peaks.empty()) return timed ? u : std::vector<float>();

    size_t next = 0;
    for (size_t i = 0; i < P; ++i) {
        const std::string name = arch.params[i].name;
        if (name.size() <= 2 || name.compare(name.size() - 2, 2, "Hz") != 0) continue;
        const float hz = peaks[next < peaks.size() ? next : peaks.size() - 1];
        ++next;
        // Invert denorm() into [0,1]. A peak outside this parameter's range
        // clamps rather than being discarded: the nearest reachable frequency
        // is still a better start than the middle of the range.
        const Param& p = arch.params[i];
        float t;
        if (p.log) {
            const float lo = std::log(std::max(1e-6f, p.lo));
            const float hi = std::log(std::max(1e-6f, p.hi));
            t = (std::log(std::max(1e-6f, hz)) - lo) / (hi - lo);
        } else {
            t = (hz - p.lo) / (p.hi - p.lo);
        }
        u[i] = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    return u;
}

std::vector<float> fit_once(const Arch& arch, const std::vector<float>& target, SfxRenderer& rend,
                            int iters, unsigned seed, float& loss_out,
                            const LossWeights& weights, const std::vector<float>* start,
                            float band_hz) {
    BandLoss loss_fn;
    loss_fn.set_target(target, weights, band_hz);
    const size_t P = arch.params.size();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.15f, 0.85f);
    std::vector<float> u(P);
    for (size_t i = 0; i < P; ++i) u[i] = seed == 0 ? 0.5f : uni(rng);
    if (start && start->size() == P) u = *start;

    auto evaluate = [&](const std::vector<float>& c) -> float {
        std::vector<float> got;
        const std::string js = build_json(arch, c);
        if (js.empty() || !rend.render(js, got)) return 1e30f;
        return loss_fn(got);
    };

    float loss = evaluate(u), step = 0.12f;
    int stalls = 0;
    for (int it = 0; it < iters; ++it) {
        // Finite differences: the engine is integer arithmetic, so there is
        // nothing to backpropagate through. One extra render per parameter
        // per step, which is affordable only because the renders are cheap.
        const float eps = std::max(0.004f, std::min(0.03f, step * 0.3f));
        std::vector<float> grad(P, 0.0f);
        for (size_t i = 0; i < P; ++i) {
            std::vector<float> up = u;
            up[i] = std::min(1.0f, up[i] + eps);
            const float d = up[i] - u[i];
            if (d <= 0) continue;
            grad[i] = (evaluate(up) - loss) / d;
        }
        float norm = 0;
        for (float g : grad) norm += g * g;
        norm = std::sqrt(norm);
        if (norm < 1e-9f) break;

        bool improved = false;
        float trial = step;
        for (int back = 0; back < 5 && !improved; ++back) {
            std::vector<float> cand(P);
            for (size_t i = 0; i < P; ++i)
                cand[i] = std::min(1.0f, std::max(0.0f, u[i] - trial * grad[i] / norm));
            const float cl = evaluate(cand);
            if (cl < loss - 1e-6f) {
                u = cand;
                loss = cl;
                improved = true;
                step = std::min(0.3f, trial * 1.5f);
            } else {
                trial *= 0.4f;
            }
        }
        if (improved) {
            stalls = 0;
        } else if (++stalls >= 8) {
            break;
        } else {
            // Stalled: nudge one knob and see if the floor moves. Cheaper
            // than a restart and it escapes most shallow basins.
            step = 0.12f;
            std::uniform_int_distribution<size_t> pick(0, P - 1);
            std::uniform_real_distribution<float> jit(-0.08f, 0.08f);
            std::vector<float> hop = u;
            const size_t j = pick(rng);
            hop[j] = std::min(1.0f, std::max(0.0f, hop[j] + jit(rng)));
            const float hl = evaluate(hop);
            if (hl < loss) {
                u = hop;
                loss = hl;
            }
        }
    }
    loss_out = loss;
    return u;
}

FitResult fit_best(const std::vector<float>& target_in, float seconds, int restarts, int iters,
                   const LossWeights& weights, const std::string& only_arch, float band_hz) {
    FitResult best;
    // One list, shared with the CLI's --list and the browser's dropdown, so
    // the search space and the menu cannot disagree about what exists.
    const std::vector<ExpandedArch> all = architectures_expanded();
    for (const ExpandedArch& e : all) {
        // `only_arch` matches either a family ("impact", every expansion of
        // it) or one expansion by its full name ("impact/brown+highpass").
        if (!only_arch.empty() && only_arch != e.name && only_arch != e.base->name) continue;

        {
            const Arch a = instantiate(e);
            const float secs = seconds > 0 ? seconds : a.seconds;

            // Size the window to what the target actually contains, not to
            // the architecture's nominal length. prepare() has already
            // trimmed the leading silence and normalised, so the extent below
            // is the sound itself.
            std::vector<float> target = target_in;
            prepare(target, static_cast<size_t>(secs * SAMPLE_RATE));
            size_t last = 0;
            for (size_t i = 0; i < target.size(); ++i)
                if (target[i] > 1e-4f || target[i] < -1e-4f) last = i;
            // A tail, so a decay is compared rather than cut; and a floor,
            // because the largest FFT the loss uses is 4096 wide.
            size_t win = last + SAMPLE_RATE / 20;
            if (win < 8192) win = 8192;
            if (win > target.size()) win = target.size();
            target.resize(win);

            SfxRenderer rend(win);

            // One EXTRA restart, begun on the target's own spectral peaks.
            //
            // Extra, not instead of. The first attempt replaced restart 0 --
            // which starts every parameter at the centre of its range -- and
            // measured WORSE on four of six references: a start with frequency
            // parameters at their extremes and everything else in the middle
            // lands in a poorer basin than a neutral start does. Adding it to
            // the pool instead can only help, because fit_best keeps the best
            // of all restarts, and it costs one restart rather than the
            // known-good one.
            const std::vector<float> seeded = peak_seeded_start(a, target, band_hz);
            const bool has_peaks = !seeded.empty() && seeded.size() == a.params.size();
            const int total = restarts + (has_peaks ? 1 : 0);

            for (int r = 0; r < total; ++r) {
                float loss = 1e30f;
                // Seed 0 starts at the centre of every range, which is the
                // best single guess; later restarts are randomised; the last
                // one, when there is one, starts on the peaks.
                const bool peak_run = has_peaks && r == total - 1;
                const std::vector<float> u =
                    fit_once(a, target, rend, iters, static_cast<unsigned>(r), loss, weights,
                             peak_run ? &seeded : nullptr, band_hz);
                ++best.restarts_run;
                if (!std::isfinite(loss)) {
                    ++best.non_finite;
                    continue;
                }
                if (loss > best.worst) best.worst = loss;
                if (loss < best.loss) {
                    best.loss = loss;
                    // The EXPANDED name, so a result names the thing that won
                    // rather than the family it came from -- "impact" tells
                    // you nothing you could reproduce.
                    best.arch = e.name;
                    best.params = u;
                    best.patch_json = build_json(a, u);
                }
            }
        }
    }
    return best;
}

}  // namespace fit
}  // namespace dsnd
