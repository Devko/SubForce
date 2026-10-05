#pragma once
// The SubForce engine: one analog-style voice in the spirit of a classic American monosynth.
//
//   Osc 1 (+ square sub) ─┐
//   Osc 2 (hard sync) ────┤ mixer ─┬─► Multidrive ─► 4-pole ladder (6/12/18/24 dB) ─► drive ─► VCA ─► out
//   Noise (white..pink..dark) ────┤ │
//                         └── feedback ◄┘ (the mixer's own output back into it, as on the Sub 37)
//
// Mono, or Duo (paraphonic: each oscillator its own key, one filter and VCA). Two DAHDSR
// envelopes (filter, amp), two mod busses (an LFO or the filter envelope to pitch, cutoff and
// one more destination), glide, note priority, single or multi trigger, analog drift.
//
// Everything from the oscillators to the VCA runs at 2x (88.2 kHz) and is folded back by a
// halfband decimator (dsp/halfband.h). Modulation and glide run every kControl samples and
// glide across them; the envelopes and the cutoff they move are computed every sample.
//
// Real-time rules: no allocation, no locks, no exceptions after construction. The plugin layer
// feeds it a Patch once per block (only when something changed).
#include "env.h"
#include "halfband.h"
#include "ladder.h"
#include "mod.h"
#include "osc.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace sf {

constexpr int kOversample = 2;
constexpr int kControl = 8;   // samples per control step (0.18 ms)

enum Slope : int { SL_6, SL_12, SL_18, SL_24 };
enum KeyMode : int { KM_MONO, KM_DUO };
enum Priority : int { PR_LAST, PR_LOW, PR_HIGH };
enum Trigger : int { TR_MULTI, TR_SINGLE };           // Single: legato notes don't restart the envelopes
enum GlideMode : int { GL_OFF, GL_ALWAYS, GL_LEGATO };
enum GlideType : int { GT_RATE, GT_TIME, GT_EXP };    // Rate: time per octave; Time: any interval; Exp: RC
enum OscDest : int { OD_BOTH, OD_OSC1, OD_OSC2 };     // which oscillators glide / take a bus's pitch
enum SubOctave : int { SO_ONE, SO_TWO };

constexpr int kOctaveMin = -2;   // 32' .. 2' (8' = 0)
constexpr int kOctaveMax = 2;
constexpr float kResMax = 4.6f;  // ladder feedback at full resonance (self-oscillation from ~4)
constexpr float kResEdge = 0.7f; // the knob where it reaches 4: "settings above 7 cause the filter
                                 // to self-oscillate" (the Sub 37's manual)

// Resonance knob 0..1 -> ladder feedback: 0..4 up to kResEdge, on to kResMax at full.
inline float resFeedback(float k) {
    return k < kResEdge ? 4.0f * k / kResEdge : 4.0f + (kResMax - 4.0f) * (k - kResEdge) / (1.0f - kResEdge);
}

struct OscPatch {
    int   octave = 0;      // kOctaveMin..kOctaveMax
    float wave = 1.0f / 3.0f;   // 0 triangle, 1/3 saw, 2/3 square, 1 narrow pulse
};

struct EnvPatch : EnvTimes {
    float vel = 0.3f;      // 0..1: how much velocity scales the envelope
    float kb = 0.0f;       // 0..1: higher notes, shorter times (1: half the time an octave up)
    bool  reset = false;   // a new note's attack starts from 0 (else from where it is)
};

struct ModPatch {
    int   src = MS_TRIANGLE;
    bool  sync = false;     // false: rateHz; true: one cycle per kSyncBeats[div] beats
    float rateHz = 5.0f;
    int   div = 9;          // 1/8
    float pitch = 0.0f;     // -1..1 (a * |a| * kModPitchRange semitones)
    int   pitchDest = OD_BOTH;
    float filter = 0.0f;    // -1..1 (a * |a| * kModCutoffRange semitones)
    int   dest = MD_OFF;
    float amount = 0.0f;    // -1..1 on `dest`
    int   control = MC_ALWAYS;
    bool  retrig = false;   // restart at each new note (else free running; synced: locked to the bar)
};

struct Patch {
    float volumeDb = 0.0f;
    OscPatch osc[2];
    float osc2Semis = 0.0f;   // Osc 2 frequency against osc 1, -7..+7 semitones
    bool  sync = false;       // Osc 2 hard-synced to osc 1
    int   subOctave = SO_ONE;
    float noiseColor = 0.5f;  // 0 white .. 0.5 pink (the Sub 37's) .. 1 dark
    bool  kbReset = false;    // oscillators restart their cycle at each new note
    float drift = 0.25f;      // 0..1 analog pitch and cutoff drift
    // Mixer, 0..1 (audio taper). Several sources up high drive the filter, as on the hardware.
    float mixOsc1 = 0.8f, mixSub = 0.0f, mixOsc2 = 0.0f, mixNoise = 0.0f, mixFeedback = 0.0f;
    // Filter
    float cutoffHz = 1500.0f;
    float res = 0.0f;         // 0..1
    float drive = 0.2f;       // Multidrive 0..1
    int   slope = SL_24;
    float keyTrack = 0.5f;    // 0..2 (1: the cutoff follows the keyboard exactly)
    float envAmount = 0.3f;   // -1..1 filter envelope amount (see kEnvRange)
    EnvPatch fenv, aenv;
    ModPatch mod[2];
    // Keyboard
    int   keyMode = KM_MONO;
    int   priority = PR_LAST;
    int   trigger = TR_MULTI;
    int   glideMode = GL_OFF;
    int   glideType = GT_TIME;
    int   glideDest = OD_BOTH;
    float glideTime = 0.08f;  // seconds (Rate: per octave)
    float bendUp = 2.0f, bendDown = 2.0f;   // semitones
};

constexpr float kEnvRange = 120.0f;   // semitones at envAmount 1 (10 octaves: 20 Hz to 20 kHz)
// envAmount a -> semitones: a gentle curve, finer near 0.
inline float envSemis(float a) { return kEnvRange * a * (0.5f + 0.5f * (a < 0.0f ? -a : a)); }

class Synth {
public:
    explicit Synth(float sampleRate = 44100.0f);

    void setPatch(const Patch& p);           // between render() calls
    void noteOn(int note, int velocity);     // velocity 0 = note off
    void noteOff(int note);
    void pitchBend(float amount);            // -1..1 (ranges in the Patch)
    void sustain(bool down);
    void allNotesOff();                      // release (CC 123)
    void reset();                            // silence now: CC 120, suspend, transport stop
    void controller(int cc, int value);      // 1 mod wheel (0..127)
    void aftertouch(float amount);           // channel pressure, 0..1
    void polyAftertouch(int note, float amount);   // a sounding key's own pressure counts as the channel's
    void resetControllers();                 // CC 121: bend, wheel, pressure and pedal back to rest
    void seed(uint32_t s);                   // the random numbers (noise, drift, S&H): per instance
    // MPC's tempo and position (quarter notes), once per block before render().
    void setTransport(double bpm, double beats, bool playing, bool beatsValid);

    void render(float* outL, float* outR, int n);   // overwrites n samples
    int  activeVoices() const;                      // 0, 1, or 2 (Duo on two notes)

    // What the voice is doing (tests, diagnostics).
    struct Info {
        bool  gate;
        int   note1, note2;       // the keys osc 1 / osc 2 play
        float pitch1, pitch2;     // their (gliding) pitch in semitones, before octave and modulation
        float filterEnv, ampEnv;
        int   held;               // keys down
        bool  silent;             // idle, not rendering
    };
    Info info() const;

    static constexpr int kHeldMax = 16;   // keys remembered for note priority

private:
    struct Ramp {   // a control value gliding across a control step
        float v = 0.0f, d = 0.0f, t = 0.0f;   // t: where it is going
        void to(float target, float invN) {
            // Arrived (the same target again, only float rounding apart): exactly there, still --
            // a still wave shape is worked out once per run instead of every sample.
            if (target == t && std::fabs(target - v) <= 1e-6f * std::fabs(target)) {
                v = target;
                d = 0.0f;
            } else {
                d = (target - v) * invN;
            }
            t = target;
        }
        void snap(float target) { v = t = target; d = 0.0f; }
        float next() { return v += d; }
        void arrive() { snap(t); }
    };
    struct Glide {
        float pitch = 60.0f, from = 60.0f, target = 60.0f;
        int   len = 0, left = 0;   // linear glides, in samples; left 0 = arrived
        bool  exp = false;
        float lk = 0.0f;           // Exp: log2 of what is left of the distance after one sample
    };
    struct Bus {
        float phase = 0.0f;
        float held = 0.0f, from = 0.0f, to = 0.0f;   // S&H / Smooth random values
        float rateMul = 1.0f;                       // from the other bus (Other Rate)
        bool  rateModulated = false;                // ...which it does: never locked to the bar
    };
    struct Drift {
        float v = 0.0f, target = 0.0f;
        int   left = 0;   // control steps to the next target
    };
    struct Key { int note, vel; };

    void holdKey(int note, int vel);
    void dropKey(int note);
    void choose(int& n1, int& n2, int& vel) const;
    void update(int pressed);
    void trigger(int vel);
    void glideTo(Glide& g, float target, bool glide);
    void stepGlide(Glide& g, int n) const;
    float busValue(Bus& b, const ModPatch& p, int n);
    void advance(int n);
    void catchUp();
    void control();
    void renderRun(float* out, int n);
    void renderSilent(float* out, int n);
    void goSilent();
    float driftStep(Drift& d, int n);
    float kbScale(float kb) const;
    // Every control value, in the order control() works out their targets.
    std::array<Ramp*, 17> ramps() {
        return {&dt_[0], &dt_[1], &wave_[0], &wave_[1], &cut_, &egAmt_, &res_, &inGain_, &postIn_, &postOut_,
                &lvl_[0], &lvl_[1], &lvl_[2], &lvl_[3], &lvl_[4], &vca_, &vol_};
    }

    float sr_, osr_, invOsr_, invSr_;
    float driftK_;            // a drift's one-pole step per sample (0.6 s)
    float cutNote_ = 0.0f;    // the cutoff knob, as a note
    Patch patch_;
    bool  havePatch_ = false;

    // keys
    Key   held_[kHeldMax] = {};
    int   nHeld_ = 0;
    bool  pedal_ = false;
    bool  gate_ = false;
    bool  havePitch_ = false;     // a note was played: Always glide has somewhere to come from
    int   note1_ = 60, note2_ = 60;
    float vel_ = 0.8f;            // the triggering key, 0..1
    float fVel_ = 1.0f;           // the filter envelope's velocity scaling for it
    float bend_ = 0.0f;
    float wheel_ = 0.0f, pressure_ = 0.0f;

    // voice
    Glide   glide_[2];
    Osc     osc_[2];
    Sub     sub_;
    Ladder  ladder_;
    Decimator dec_;
    Env     fenv_, aenv_;
    EnvCoef fc_, ac_;
    uint32_t rng_ = 0x2545F491u, noiseRng_ = 0x9E3779B9u;
    float   noiseLp_ = 0.0f, noiseHp_ = 0.0f, pink_[3] = {};   // noiseHp_: the 30 Hz high-pass's low part
    float   fbIn_ = 0.0f, fbX1_ = 0.0f, fbY1_ = 0.0f;   // feedback: the mixer's output, overloaded,
    float   fbLp_ = 0.0f;                                // band-limited and DC-blocked, a sample late
    float   dcX1_ = 0.0f, dcY1_ = 0.0f;                  // output DC blocker
    float   fPrev_ = 0.1f, aePrev_ = 0.0f;               // last base sample's cutoff coefficient and VCA
    bool    fPrevValid_ = false;                         // fPrev_ is from this note's sound (not before a silence)
    int     tap_ = SL_24, tapFrom_ = SL_24, xfLeft_ = 0; // the slope's ladder tap, crossfading from tapFrom_
    bool    resetPending_ = false;                       // keyboard reset at the next sample
    bool    silent_ = true;
    int     idleSteps_ = 0;
    float   peak_ = 0.0f;                                // |output| since the last control step

    // control
    int   ctlLeft_ = 0;       // samples left in this control step (0: compute one now)
    int   sinceCtl_ = 0;      // samples since the last one
    bool  snap_ = false;      // pitch jumps: no glide from the old values
    bool  snapAll_ = true;    // the first step: every value starts at its target
    Ramp  dt_[2], wave_[2], cut_, egAmt_, res_, inGain_, postIn_, postOut_, lvl_[5], vca_, vol_;
    Bus   bus_[2];
    float busOut_[2] = {};    // the busses' sources at the last control-rate time
    float driftNow_[3] = {};
    Drift drift_[3];          // osc 1, osc 2, cutoff
    float noteDrift_[2] = {}; // per-note offsets, cents
    float noiseK_ = 1.0f, noisePink_ = 1.0f, noiseComp_ = 1.0f;   // colour: dark one-pole, pink mix, level
    float driveBias_ = 0.0f;   // Multidrive's asymmetry (its tube-like even harmonics)
    double beats_ = 0.0, bpm_ = 120.0, beatsPerSample_ = 120.0 / 60.0 / 44100.0;
    bool  playing_ = false, beatsValid_ = false;
};

} // namespace sf
