#pragma once
// The two mod busses' vocabulary: sources, the programmable destination, what scales the depth,
// and the tempo-synced rates. surface/surface.py lists the same names in the same order
// (plugin/patch_map.cpp static_asserts the counts).
namespace sf {

// Saw falls, Ramp rises. S&H: a new random value every cycle; Smooth: glides between them.
enum ModSource : int { MS_TRIANGLE, MS_SQUARE, MS_SAW, MS_RAMP, MS_SAMPLE_HOLD, MS_SMOOTH, MS_FILTER_EG, MS_COUNT };

// Besides pitch and cutoff (each bus has its own amounts for those), one more destination.
// Other Rate: the other bus's rate (x1/16 .. x16).
enum ModDest : int { MD_OFF, MD_WAVE, MD_WAVE1, MD_WAVE2, MD_RES, MD_DRIVE, MD_SUB, MD_NOISE, MD_FEEDBACK, MD_VOLUME,
                     MD_OTHER_RATE, MD_COUNT };

// What sets the bus's depth: nothing (always full), the mod wheel, channel pressure, velocity.
enum ModControl : int { MC_ALWAYS, MC_MODWHEEL, MC_AFTERTOUCH, MC_VELOCITY, MC_COUNT };

// Tempo-synced rates, in beats (quarter notes). Double: a float triplet drifts off MPC's grid.
constexpr int kNumSyncDivs = 17;
constexpr double kSyncBeats[kNumSyncDivs] = {32.0, 16.0, 8.0, 4.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0, 1.5,
                                             0.5, 1.0 / 3.0, 0.75, 0.25, 1.0 / 6.0, 0.375, 0.125, 1.0 / 12.0};

// Amount curves (amount a in -1..1 -> a * |a| * range: fine near 0, wide at the ends).
constexpr float kModPitchRange = 24.0f;    // semitones
constexpr float kModCutoffRange = 60.0f;   // semitones (5 octaves)
constexpr float kOtherRateOctaves = 4.0f;  // Other Rate: x2^-4 .. x2^4

} // namespace sf
