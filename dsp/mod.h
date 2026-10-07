#pragma once
// The two mod busses' vocabulary: sources, the programmable destination, what scales the depth,
// and the tempo-synced rates. surface/surface.py lists the same names in the same order
// (plugin/patch_map.cpp static_asserts the counts). Saved state keeps an option's index: new
// options go at the end of a list.
namespace sf {

// Saw falls, Ramp rises. S&H: a new random value every cycle; Smooth: glides between them;
// Noise: random, its bandwidth the rate. The LFO shapes run -1..1, the rest 0..1 except Key
// (the gliding key against C3, kKeySpan semitones a unit, either way).
enum ModSource : int { MS_TRIANGLE, MS_SQUARE, MS_SAW, MS_RAMP, MS_SAMPLE_HOLD, MS_SMOOTH, MS_FILTER_EG,
                       MS_SINE, MS_NOISE, MS_AMP_EG, MS_VELOCITY, MS_AFTERTOUCH, MS_KEY, MS_CONSTANT, MS_COUNT };
// The sources the rate moves.
constexpr bool isLfo(int s) { return s <= MS_SMOOTH || s == MS_SINE || s == MS_NOISE; }

// Besides pitch and cutoff (each bus has its own amounts for those), one more destination.
// Other Rate: the other bus's rate (x1/16 .. x16). The times: x1/8 .. x8 (EG: both envelopes;
// Glide: each glide as it starts).
enum ModDest : int { MD_OFF, MD_WAVE, MD_WAVE1, MD_WAVE2, MD_RES, MD_DRIVE, MD_SUB, MD_NOISE, MD_FEEDBACK, MD_VOLUME,
                     MD_OTHER_RATE, MD_EG_AMOUNT, MD_KEY_TRACK, MD_OSC1, MD_OSC2, MD_BEAT, MD_EG_TIME, MD_FEG_TIME,
                     MD_AEG_TIME, MD_GLIDE, MD_COUNT };

// What sets the bus's depth: nothing (always full), the mod wheel, channel pressure, velocity,
// or None (only the depth amounts: ModPatch wheel / vel / at, added to any of them).
enum ModControl : int { MC_ALWAYS, MC_MODWHEEL, MC_AFTERTOUCH, MC_VELOCITY, MC_NONE, MC_COUNT };

// Free: rateHz; Sync: MPC's tempo; Hi: rateHz x kHiRange, worked out every sample (audio-rate
// modulation of pitch, cutoff, wave and volume), the original's HI RANGE.
enum RateMode : int { RM_FREE, RM_SYNC, RM_HI, RM_COUNT };
constexpr float kHiRange = 10.0f;

// Tempo-synced rates, in beats (quarter notes). Double: a float triplet drifts off MPC's grid.
constexpr int kNumSyncDivs = 17;
constexpr double kSyncBeats[kNumSyncDivs] = {32.0, 16.0, 8.0, 4.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0, 1.5,
                                             0.5, 1.0 / 3.0, 0.75, 0.25, 1.0 / 6.0, 0.375, 0.125, 1.0 / 12.0};
// The same as cycles per beat: the audio thread multiplies (a double division costs ~30 cycles).
constexpr double kSyncPerBeat[kNumSyncDivs] = {1.0 / 32.0, 1.0 / 16.0, 0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0 / 3.0,
                                               2.0, 3.0, 4.0 / 3.0, 4.0, 6.0, 8.0 / 3.0, 8.0, 12.0};
constexpr bool syncTablesAgree() {
    for (int i = 0; i < kNumSyncDivs; ++i) {
        const double p = kSyncBeats[i] * kSyncPerBeat[i];
        if (p < 1.0 - 1e-12 || p > 1.0 + 1e-12) return false;
    }
    return true;
}
static_assert(syncTablesAgree(), "kSyncPerBeat is 1 / kSyncBeats");

// Amount curves (amount a in -1..1 -> a * |a| * range: fine near 0, wide at the ends).
constexpr float kModPitchRange = 24.0f;    // semitones
constexpr float kModCutoffRange = 60.0f;   // semitones (5 octaves)
// The programmable destination: amount a in -1..1, linear.
constexpr float kOtherRateOctaves = 4.0f;  // Other Rate: x2^-4 .. x2^4
constexpr float kModTimeOctaves = 3.0f;    // EG and glide times: x2^-3 .. x2^3
constexpr float kKeySpan = 24.0f;          // the Key source: 1 two octaves above C3 (MIDI 60)

} // namespace sf
