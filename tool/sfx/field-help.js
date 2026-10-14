//
// One sentence per field, shown in the policy inspector.
//
// Separate from proj.js's table because these are prose that will be edited by
// someone reading the UI, and the table is numbers that must match
// src/sfx/sfx_json.cpp. Mixing the two makes the numbers harder to audit.
//
// These say what a field is FOR, not what it is -- "gain, in decibels" helps
// nobody standing in front of a control labelled "gain" with "dB" beside it.
// Where docs/SFX_FORMAT.md gives a reason, that reason is the sentence.
//
export const FIELD_HELP = {
  // --- bus -----------------------------------------------------------------
  gainDb: 'Applied to everything on this bus, and rampable while the game runs.',
  maxInstances: 'The budget. Stealing never crosses a bus, so a world flood '
    + 'cannot silence the UI or the player\'s own weapon.',
  spatial: 'Off means the position argument is ignored and pan is taken directly '
    + '— UI, the player\'s own weapon, anything not in the world. A spatial bus '
    + 'also caches mono, because a point source\'s own stereo image fights the panner.',
  sendReverb: 'How much of this bus reaches the reverb.',
  sendChorus: 'How much of this bus reaches the chorus. This is the control that '
    + 'turns one synthesized voice into several — a choir rather than a singer, '
    + 'a machine that beats rather than drones.',
  sendDelay: 'How much of this bus reaches the delay.',

  // --- bank effects --------------------------------------------------------
  'fx.reverb.roomSize': 'How large the space is.',
  'fx.reverb.damping': 'How fast the high frequencies die in it. More damping is '
    + 'a softer, more absorbent room.',
  'fx.reverb.width': 'Stereo spread of the tail.',
  'fx.reverb.wetGain': 'Zero switches the effect off entirely — it is not fed, and '
    + 'it does not run.',
  'fx.reverb.preDelayMs': 'A gap before the tail starts. A little of it keeps the '
    + 'transient dry and readable.',
  'fx.reverb.highPassHz': 'Keeps low frequencies out of the tail, so an explosion '
    + 'does not turn the room to mud.',

  'fx.chorus.rateHz': 'How fast the detuning sweeps. Slow is a choir breathing; '
    + 'fast is a rotary speaker.',
  'fx.chorus.depthMs': 'How far it detunes. More depth is more voices, and past a '
    + 'point, seasickness.',
  'fx.chorus.wetGain': 'Zero switches the effect off entirely.',

  'fx.delay.timeMs': 'The gap between repeats.',
  'fx.delay.feedback': 'How much of each repeat feeds the next. Near the top it '
    + 'runs away.',
  'fx.delay.wetGain': 'Zero switches the effect off entirely.',
  'fx.delay.pingPong': 'Repeats alternate between the speakers.',

  // --- sound ---------------------------------------------------------------
  priority: 'Higher survives. A higher-priority instance is never cut for a '
    + 'lower-priority one — the new play is refused instead.',
  onLimit: 'What happens at the cap. stealOldest cuts this sound\'s own oldest, '
    + 'never anyone else\'s.',
  cooldownMs: 'Minimum gap between accepted triggers. Eight monsters firing the '
    + 'same pistol in one tick is one trigger and seven rejections, none of which '
    + 'touches the voice pool.',
  pitchJitterCents: '± per trigger. Above 600 forces this sound live. Note that '
    + 'the noise oscillator ignores pitch, so this does nothing to a live noise '
    + 'burst but shifts the whole spectrum of a cached one — use gain jitter for noise.',
  gainJitterDb: '± per trigger. The right variation for noise-based sounds.',
  live: 'Synthesize per instance instead of playing cached PCM. Costs a voice '
    + 'every time it plays; in exchange its noise varies per trigger.',
  loop: 'Implies live — a loop has no finite render.',

  // --- attenuation ---------------------------------------------------------
  'attenuation.minDist': 'Full gain within this. Pan also collapses toward centre '
    + 'as the listener enters the source, or walking through a sound swings it '
    + 'ear to ear over a few centimetres.',
  'attenuation.maxDist': 'Silent beyond this. The default pair is Doom\'s own '
    + 'S_CLOSE_DIST and S_CLIPPING_DIST at this engine\'s scale, so a converted '
    + 'map sounds the way its author placed it.',
  'attenuation.curve': 'The same curve tables envelopes use, so "exp" means one '
    + 'thing across the project.',
  'attenuation.spread': 'Scales pan width. Zero is always centre.',
  'attenuation.rearDb': 'Attenuation for a source directly behind. This is '
    + 'disambiguation, not a cue: stereo cannot tell front from back, and a few '
    + 'dB separates them without pretending to be HRTF.',
  'attenuation.airLpfHz': 'The low-pass corner reached at max distance. Off at zero.',

  // --- the modal body ------------------------------------------------------
  hz: 'Where this mode rings. A struck object\'s first few modes carry its '
    + 'identity — its size, its material, whether it is hollow.',
  decayMs: 'How long this mode takes to fall 60 dB. Long is metal, short is wood, '
    + 'very short is a dull thud.',
  gain: 'How loud this mode rings when struck, relative to the strike. It is '
    + 'the ring\'s amplitude, not a filter gain: a long mode fed a sustained '
    + 'tone builds far past it, as a real body does.',
  resonatorDry: 'How much of the exciter survives under the body. Zero is the '
    + 'resonance alone with no click; around one keeps the strike audible, which '
    + 'is usually what a real impact sounds like.',

  // --- ducking -------------------------------------------------------------
  'duckMusic.db': 'How far to pull the music down. At most four requests are '
    + 'honoured at once and the engine takes the minimum — a bank where '
    + 'everything ducks is a bank where nothing stands out.',
  'duckMusic.ms': 'How long the duck lasts. Zero discards the duck entirely, '
    + 'however large the dB figure.',

  // --- variant / gesture ---------------------------------------------------
  weight: 'Relative odds when the caller does not name a variant. These are odds, '
    + 'not percentages.',
  atMs: 'When this step fires, from the start of the sound.',
  note: 'Pitch for this step.',
  vel: 'Velocity for this step.',
  durMs: 'How long the note is held. Zero means no note-off at all — the envelope '
    + 'ends the voice, which is what oneShot is for and is the common case.',
};
