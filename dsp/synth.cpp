#include "synth.h"

#include "stages.h"

#include <algorithm>
#include <cmath>

namespace sf {

#ifdef SF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

#ifdef SF_TUNE
// The calibration build (tools/demos as build/demos_tune): these constants can be set by name between
// renders (tuneSet), so an optimiser can fit them to recordings. The plugin keeps them constexpr.
#define SF_TUNABLE(name, value) float name = value
#else
#define SF_TUNABLE(name, value) constexpr float name = value
#endif

namespace {

// Multidrive's two stages. 0.0.7 calibrated them against a recording of the original's factory
// presets (sounds at full Multidrive, the decoded knob positions played on the recording's notes): the
// original is grittier there; a fit asked for the clipper's gain at full x1.6 and the drive into the
// ladder x1.24, of which about half was taken (the clipper 1.5 -> 2.0, the ladder 8x -> 9x), the
// evidence being two sounds.
SF_TUNABLE(kInGain, 0.5f);               // mixer -> ladder at Multidrive 0 (one oscillator at full: mild warmth)
SF_TUNABLE(kDriveSpan, 8.0f);            // Multidrive 1: 9x that (8x until 0.0.7)
SF_TUNABLE(kPostBase, 0.5f);             // Multidrive's second stage: its clipper's input gain at drive 0..
SF_TUNABLE(kPostSpan, 1.5f);             // ...rising by this at full, 4x (3x until 0.0.7); its output takes the inverse back
// The feedback loop (the mixer's output back into it), tuned on renders of a saw through it: from
// a slight thickening (+1 dB at half the knob) and more drive into the filter, through grit, to the
// chaos of a loop over unity gain in the last tenth (+5 dB, the upper harmonics +15 dB).
SF_TUNABLE(kFeedbackGain, 1.4f);         // the loop's gain at full: unity at 85% of the knob
SF_TUNABLE(kFeedbackClip, 0.7f);         // where the loop's own stage (the EXT IN level amp) saturates
constexpr float kFeedbackHp = 150.0f;    // Hz: AC coupled: no bass builds up around the loop
constexpr float kFeedbackLp = 7000.0f;   // Hz: the loop's bandwidth, an analog stage's
SF_TUNABLE(kDriveBias, 0.3f);            // Multidrive's asymmetry at its middle (tube-like); subtle low, none at the ends
constexpr float kOutGain = 1.51f;        // engine output at 0 dB volume (1.2 until 0.0.1: +2 dB, level with MPC's own instruments)
constexpr float kMaxCutoff = 0.40f;      // of the 2x rate (35 kHz): the ladder's coefficient stays sane
constexpr float kMinCutoff = 8.0f;       // Hz
constexpr int kTapFade = 32;             // samples to crossfade a slope change
constexpr float kThermal = 1e-4f;        // the ladder's input noise floor (-80 dB)

// Analog (the knob that was Drift), at full. Modelled on how an analog oscillator works (a ramp
// core reset by a transistor, the other shapes made from it, AC coupled into the mixer) and kept
// small: no unit to measure, so nothing a careful listener would call a fault.
constexpr float kAnaJitter = 0.0006f;    // each cycle's length, relative: about +-1 cent
constexpr float kAnaBow = 0.06f, kAnaBowTol = 0.03f;   // the ramp's bow, and how much units differ in it
constexpr float kAnaPw = 0.012f;         // the comparator's error: the square's width +-1.2% of the cycle
constexpr float kAnaBw = 40000.0f;       // Hz: the oscillators' bandwidth, Analog 0..1: 40 kHz down to
constexpr float kAnaBwDrop = 20000.0f;   //     about 20 kHz (edges and tips a little soft)
constexpr float kAnaCoupling = 3.0f;     // Hz: their coupling into the mixer (a square sags at bass notes)
constexpr float kAnaDetune = 1.5f;       // cents: the unit's tuning error, per oscillator
constexpr float kAnaCut = 0.25f, kAnaRes = 0.015f, kAnaEnv = 0.03f;   // the unit's cutoff (semitones),
                                                                      // resonance (knob), envelope times
constexpr float kAnaFast = 1.2f, kAnaFastCut = 0.12f;   // the faster drift: cents, semitones
constexpr float kAnaNoteWave = 0.012f, kAnaNoteCut = 0.3f, kAnaNoteRes = 0.012f;   // per note: wave knob,
                                                                                    // semitones, resonance knob

// Mixer knobs: an audio taper.
inline float taper(float k) {
    k = clampf(k, 0.0f, 1.0f);
    return k * k;
}

inline float noteOf(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

// The Noise source: white noise (a new value each step) through a one-pole at the bus's rate.
// noisePole: the step for a bandwidth of hz over `samples` (1 - e^-x, its series for small x: no
// cancellation at the slowest rates). noiseGain: what brings the one-pole's output (uniform white
// in, rms 1/sqrt(3) * sqrt(k / (2 - k))) to kNoiseRms, whatever the rate: within -1..1 all but
// about 3% of the time (it is clipped there).
constexpr float kNoiseRms = 0.45f;
inline float noisePole(float hz, float samples, float invSr) {
    const float x = std::min(2.0f * kPi * hz * samples * invSr, 8.0f);
    return x < 1e-3f ? x - 0.5f * x * x : 1.0f - exp2Fast(-1.442695041f * x);
}
inline float noiseGain(float k) { return kNoiseRms * 1.732050808f * std::sqrt((2.0f - k) / (k > 1e-9f ? k : 1e-9f)); }

// Pink noise at the 2x rate: Paul Kellet's "economy" filter (three one-poles and a direct term,
// within about 1 dB of -3 dB/oct from 10 Hz up), its corners moved to 88.2 kHz.
constexpr float kPinkPole[3] = {0.998824309f, 0.981325634f, 0.754983444f};
constexpr float kPinkGain[3] = {0.049552129f, 0.149655561f, 0.599829761f};
constexpr float kPinkDirect = 0.1848f;
constexpr float kNoiseHp = 0.002134856f;   // a one-pole high-pass at 30 Hz: no infrasonic rumble
// The noise level that keeps every colour as loud as white was (RMS through the wide-open ladder,
// the decimator and the output's DC blocker), colour 0..1 in 16 steps: white, toward pink at 0.5,
// then darker. From the filters' responses.
constexpr float kNoiseComp[17] = {2.000000f, 1.728349f, 1.428936f, 1.182656f, 0.994840f, 0.852474f, 0.742894f,
                                  0.656807f, 0.587788f, 0.599895f, 0.621496f, 0.652295f, 0.691527f, 0.739680f,
                                  0.800082f, 0.879364f, 0.987116f};

} // namespace

#ifdef SF_TUNE
bool tuneSet(const std::string& name, float v) {
    struct { const char* name; float* at; } const all[] = {
        {"kInGain", &kInGain}, {"kDriveSpan", &kDriveSpan}, {"kPostBase", &kPostBase}, {"kPostSpan", &kPostSpan},
        {"kFeedbackGain", &kFeedbackGain}, {"kFeedbackClip", &kFeedbackClip}, {"kDriveBias", &kDriveBias},
        {"kResMax", &kResMax}, {"kResCurve", &kResCurve}, {"kResEdge", &kResEdge}};
    for (const auto& t : all)
        if (name == t.name) return *t.at = v, true;
    return false;
}
#endif

Synth::Synth(float sampleRate)
    : sr_(sampleRate), osr_(sampleRate * kOversample), invOsr_(1.0f / (sampleRate * kOversample)),
      invSr_(1.0f / sampleRate), driftK_(1.0f / (0.6f * sampleRate)),
      fbK_(1.0f - std::exp(-2.0f * kPi * kFeedbackLp / (sampleRate * kOversample))) {
    setTransport(120.0, 0.0, false, false);
    setPatch(Patch{});
}

void Synth::setPatch(const Patch& p) {
    const bool repick = havePatch_ && (p.keyMode != patch_.keyMode || p.priority != patch_.priority ||
                                       p.osc2Keys != patch_.osc2Keys);
    // A latch let go with no key down: that envelope releases now.
    const bool unlatch[2] = {havePatch_ && patch_.fenv.latch && !p.fenv.latch, havePatch_ && patch_.aenv.latch && !p.aenv.latch};
    patch_ = p;
    if (!gate_) {
        if (unlatch[0]) fenv_.release();
        if (unlatch[1]) aenv_.release();
    }
    analogSetup();
    havePatch_ = true;
    cutNote_ = noteOf(p.cutoffHz);
    // A bus whose rate the other one modulates runs free even when synced (a locked phase can't
    // speed up and slow down).
    for (int b = 0; b < 2; ++b) {
        bus_[b].rateModulated = p.mod[1 - b].dest == MD_OTHER_RATE && p.mod[1 - b].amount != 0.0f;
        fast_[b] = p.mod[b].hi && isLfo(p.mod[b].src);
    }
    // Mono <-> Duo or another priority with keys down: the oscillators take the keys the new rule
    // picks, as a legato move (no new attack, no glide).
    if (repick && gate_ && nHeld_ > 0) {
        int n1 = 0, n2 = 0, vel = 0;
        choose(n1, n2, vel);
        if (n1 != note1_ || n2 != note2_) {
            note1_ = n1;
            note2_ = n2;
            glideTo(glide_[0], static_cast<float>(n1), false);
            glideTo(glide_[1], static_cast<float>(n2), false);
            ctlLeft_ = 0;
        }
    }
    envCoefs();
    // Noise colour: white crossfading to pink up to 0.5, then pink through a one-pole down to ~220
    // Hz; the same RMS at every colour (kNoiseComp).
    const float nc = clampf(p.noiseColor, 0.0f, 1.0f);
    noisePink_ = std::min(1.0f, 2.0f * nc);
    noiseK_ = nc > 0.5f ? exp2Fast(-6.0f * (2.0f * nc - 1.0f)) : 1.0f;
    const float at = nc * 16.0f;
    const int i0 = std::min(static_cast<int>(at), 15);
    noiseComp_ = kNoiseComp[i0] + (kNoiseComp[i0 + 1] - kNoiseComp[i0]) * (at - static_cast<float>(i0));
}

float Synth::kbScale(float kb) const { return exp2Fast(-clampf(kb, 0.0f, 1.0f) * (static_cast<float>(note1_) - kKeyCentre) / 12.0f); }

// The envelopes' coefficients: their times, keyboard tracking (from the key playing) and the
// busses' EG Time.
void Synth::envCoefs(bool fast) {
    fc_ = envCoef(patch_.fenv, sr_, kbScale(patch_.fenv.kb) * egTimeMul_[0] * an_.env[0], fast);
    ac_ = envCoef(patch_.aenv, sr_, kbScale(patch_.aenv.kb) * egTimeMul_[1] * an_.env[1], fast);
}

// Analog: the unit's tolerances (from its number: the same unit, the same instrument) scaled by the
// knob, and what they do to the oscillators. At 0 everything here is exactly neutral.
void Synth::analogSetup() {
    if (patch_.unit != unitMade_) {
        uint32_t h = patch_.unit * 0x9E3779B9u ^ 0x85EBCA6Bu;
        if (!h) h = 1u;
        for (int k = 0; k < 4; ++k) xorshift(h);
        for (float& t : unitTol_) t = randBipolar(h);
        unitMade_ = patch_.unit;
    }
    const float a = clampf(patch_.drift, 0.0f, 1.0f);
    const float* u = unitTol_;
    if (a > 0.0f && !an_.on) oscLp_ = hpX_ = hpY_ = 0.0f;   // coming back on: no stale state
    an_.on = a > 0.0f;
    for (int o = 0; o < 2; ++o) {
        an_.pwOff[o] = a * kAnaPw * u[U_PW1 + o];
        an_.bow[o] = a * (kAnaBow + kAnaBowTol * u[U_BOW1 + o]);
        an_.cents[o] = a * kAnaDetune * u[U_DET1 + o];
    }
    const float bw = kAnaBw - a * kAnaBwDrop * (1.0f + 0.15f * u[U_LP]);
    an_.lpK = 1.0f - std::exp(-2.0f * kPi * bw * invOsr_);
    an_.hpR = 1.0f - 2.0f * kPi * a * kAnaCoupling * (1.0f + 0.3f * u[U_HP]) * invOsr_;
    an_.cut = a * kAnaCut * u[U_CUT];
    an_.res = a * kAnaRes * u[U_RES];
    an_.env[0] = 1.0f + a * kAnaEnv * u[U_ENVF];
    an_.env[1] = 1.0f + a * kAnaEnv * u[U_ENVA];
    an_.jitter = a * kAnaJitter;
}

// --- keys ---------------------------------------------------------------------------------

void Synth::holdKey(int note, int vel) {
    dropKey(note);
    if (nHeld_ == kHeldMax) {   // forget the oldest key
        for (int i = 1; i < kHeldMax; ++i) held_[i - 1] = held_[i];
        --nHeld_;
    }
    held_[nHeld_++] = {note, vel};
}

void Synth::dropKey(int note) {
    int w = 0;
    for (int i = 0; i < nHeld_; ++i)
        if (held_[i].note != note) held_[w++] = held_[i];
    nHeld_ = w;
}

// The key oscillator 1 plays (by priority) and, in Duo, oscillator 2's: the next one by the
// same rule (the same key when only one is down).
void Synth::choose(int& n1, int& n2, int& vel) const {
    auto better = [this](const Key& a, const Key& b) {
        return patch_.priority == PR_LOW ? a.note < b.note : a.note > b.note;
    };
    int k1 = nHeld_ - 1, k2 = -1;   // Last: the newest, then the one before it
    if (patch_.priority == PR_LAST) {
        k2 = nHeld_ - 2;
    } else {
        for (int i = nHeld_ - 2; i >= 0; --i)   // newest first: ties go to the newer key
            if (better(held_[i], held_[k1])) k1 = i;
        for (int i = nHeld_ - 1; i >= 0; --i)
            if (i != k1 && (k2 < 0 || better(held_[i], held_[k2]))) k2 = i;
    }
    n1 = held_[k1].note;
    vel = held_[k1].vel;
    n2 = patch_.keyMode == KM_DUO && k2 >= 0 ? held_[k2].note : n1;
    // Duo, oscillator 2 on the highest (or lowest) key, oscillator 1 on the other end; the newest
    // key's velocity.
    if (patch_.keyMode == KM_DUO && nHeld_ > 1 && (patch_.osc2Keys == O2_HIGH || patch_.osc2Keys == O2_LOW)) {
        int lo = 0, hi = 0;
        for (int i = 1; i < nHeld_; ++i) {
            if (held_[i].note < held_[lo].note) lo = i;
            if (held_[i].note > held_[hi].note) hi = i;
        }
        const bool high = patch_.osc2Keys == O2_HIGH;
        n1 = held_[high ? lo : hi].note;
        n2 = held_[high ? hi : lo].note;
        vel = held_[nHeld_ - 1].vel;
    }
    if (patch_.osc2Keys == O2_DRONE) n2 = n1;   // a drone takes no key: Duo plays as Mono
}

void Synth::noteOn(int note, int velocity) {
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    note = std::clamp(note, 0, 127);
    holdKey(note, velocity);
    update(note);
}

void Synth::noteOff(int note) {
    const int before = nHeld_;
    dropKey(note);
    if (nHeld_ != before) update(-1);
}

// The keys changed (`pressed`: the key just struck, -1 for a release): move the oscillators,
// open or close the gate, retrigger as the trigger mode says. Legato (another key was down):
// Single keeps the envelopes going; Legato glide glides only then.
void Synth::update(int pressed) {
    catchUp();   // glides and busses move on from where they really are at this sample
    if (nHeld_ == 0) {
        if (gate_ && !pedal_) {   // the pedal keeps the last notes sounding until it lifts
            gate_ = false;
            releaseEnvs();
        }
        return;
    }
    int n1 = 0, n2 = 0, vel = 0;
    choose(n1, n2, vel);
    const bool fresh = !gate_;
    // A sounding key struck again (the pedal held it, or a repeat) restarts it in Multi.
    const bool restrike = pressed >= 0 && (pressed == n1 || pressed == n2) && patch_.trigger == TR_MULTI;
    if (!fresh && !restrike && n1 == note1_ && n2 == note2_) return;   // the sounding keys didn't change
    // Always glides from the last note played; the very first note has none to come from.
    const bool glide = havePitch_ && (patch_.glideMode == GL_ALWAYS || (patch_.glideMode == GL_LEGATO && !fresh));
    havePitch_ = true;
    note1_ = n1;
    note2_ = n2;
    // Multi retriggers on a key struck, not on a release that hands the oscillators back to a
    // key still held (a trill would attack twice, a Duo pair's other key on every lift).
    const bool attack = fresh || (patch_.trigger == TR_MULTI && pressed >= 0);
    if (attack) vel_ = static_cast<float>(std::clamp(vel, 1, 127)) / 127.0f;   // this note's, for its glide
    noteTimeMods();   // Glide Time and EG Time as they are now, not at the last control step
    glideTo(glide_[0], static_cast<float>(n1), glide && patch_.glideDest != OD_OSC2);
    glideTo(glide_[1], static_cast<float>(n2), glide && patch_.glideDest != OD_OSC1);
    gate_ = true;
    if (attack) trigger(vel);
    ctlLeft_ = 0;   // the new pitch from the next sample on
}

void Synth::trigger(int vel) {
    vel_ = static_cast<float>(std::clamp(vel, 1, 127)) / 127.0f;
    envCoefs();
    // EG Sync counts its units from here (or from the bar while MPC plays). A note a control step
    // or less before a unit's start belongs to that unit: no restart a few samples into it.
    for (int e = 0; e < 2; ++e) {
        const int s = (e ? patch_.aenv : patch_.fenv).sync;
        envBeats_[e] = 0.0;
        if (s <= 0) continue;
        const double per = kSyncPerBeat[std::min(s - 1, kNumSyncDivs - 1)];
        const double at = playing_ && beatsValid_ ? beats_ : 0.0;
        envCycle_[e] = floorFast(at * per);
        envSkip_[e] = floorFast((at + static_cast<double>(kControl) * beatsPerSample_) * per) != envCycle_[e];
    }
    fenv_.trigger(fc_, patch_.fenv.reset);
    aenv_.trigger(ac_, patch_.aenv.reset);
    for (int b = 0; b < 2; ++b)
        if (patch_.mod[b].retrig) {
            bus_[b].phase = 0.0f;
            newCycle(bus_[b]);
        }
    if (patch_.kbReset) resetPending_ = true;
    // The note's own offsets, -1..1 (control() scales them by the knob as it is then): pitch, and
    // Analog's (its own random numbers: at 0 nothing else changes) shape, cutoff and resonance.
    for (float& d : noteDrift_) d = randBipolar(rng_);
    for (float& w : noteWave_) w = randBipolar(anaRng_);
    noteCut_ = randBipolar(anaRng_);
    noteRes_ = randBipolar(anaRng_);
    if (silent_) {   // waking: nothing to glide from, every value starts where it belongs
        silent_ = false;
        snapAll_ = true;
        aePrev_ = 0.0f;
        fPrevValid_ = false;
    }
    idleSteps_ = 0;
}

void Synth::glideTo(Glide& g, float target, bool glide) {
    g.target = target;
    const float dist = std::fabs(target - g.pitch);
    const float time = patch_.glideTime * glideMul_;   // Glide Time from the busses, as it starts
    if (!glide || dist < 1e-4f || time < 1e-4f) {
        g.pitch = target;
        g.left = 0;
        snap_ = true;
        return;
    }
    if (patch_.glideType == GT_EXP) {   // RC: 99% of the way at glideTime (e^-4.6)
        g.exp = true;
        g.lk = -4.6f / (time * sr_ * 0.693147181f);
        g.left = 1;
        return;
    }
    // Counted in samples, not summed steps: a slow, small glide never stalls on rounding.
    const float samples = time * sr_ * (patch_.glideType == GT_RATE ? dist / 12.0f : 1.0f);
    g.exp = false;
    g.from = g.pitch;
    g.len = g.left = static_cast<int>(std::clamp(samples, 1.0f, 1e9f));
}

void Synth::stepGlide(Glide& g, int n) const {
    if (g.left <= 0 || (patch_.glideGated && !gate_)) return;   // Gated: the glide waits for a key
    if (g.exp) {
        g.pitch += (g.target - g.pitch) * (1.0f - exp2Fast(g.lk * static_cast<float>(n)));
        if (std::fabs(g.target - g.pitch) < 1e-3f) {
            g.pitch = g.target;
            g.left = 0;
        }
        return;
    }
    g.left = std::max(0, g.left - n);
    g.pitch = g.target - (g.target - g.from) * static_cast<float>(g.left) / static_cast<float>(g.len);
}

void Synth::pitchBend(float amount) { bend_ = clampf(amount, -1.0f, 1.0f); }

void Synth::sustain(bool down) {
    pedal_ = down;
    if (!down && nHeld_ == 0 && gate_) {
        gate_ = false;
        releaseEnvs();
    }
}

void Synth::allNotesOff() {   // CC 123: every note lets go, latched envelopes too
    nHeld_ = 0;
    gate_ = false;
    fenv_.release();
    aenv_.release();
}

// The keys let go: each envelope releases, unless it is latched (reset() still silences it).
void Synth::releaseEnvs() {
    if (!patch_.fenv.latch) fenv_.release();
    if (!patch_.aenv.latch) aenv_.release();
}

void Synth::reset() {
    catchUp();   // the samples rendered since the last control step count once, now
    nHeld_ = 0;
    pedal_ = gate_ = false;
    fenv_ = Env{};
    aenv_ = Env{};
    resetPending_ = false;
    for (Glide& g : glide_) {
        g.pitch = g.target;
        g.left = 0;
    }
    goSilent();
}

void Synth::controller(int cc, int value) {
    if (cc == 1) wheel_ = static_cast<float>(std::clamp(value, 0, 127)) / 127.0f;
}

void Synth::aftertouch(float amount) { pressure_ = clampf(amount, 0.0f, 1.0f); }

void Synth::polyAftertouch(int note, float amount) {
    if (nHeld_ > 0 && (note == note1_ || (patch_.keyMode == KM_DUO && note == note2_))) aftertouch(amount);
}

void Synth::resetControllers() {
    bend_ = wheel_ = pressure_ = 0.0f;
    sustain(false);
}

void Synth::seed(uint32_t s) {
    rng_ = s ? s : 1u;
    noiseRng_ = s * 0x9E3779B9u + 0x2545F491u;
    if (!noiseRng_) noiseRng_ = 1u;
    anaRng_ = (s ^ 0x6C8E9CF5u) * 0x85EBCA6Bu;
    if (!anaRng_) anaRng_ = 1u;
    jitRng_ = (s ^ 0x3C6EF372u) * 0xC2B2AE35u;
    if (!jitRng_) jitRng_ = 1u;
}

void Synth::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
    beatsPerSample_ = bpm_ / 60.0 / static_cast<double>(sr_);
    const bool was = playing_ && beatsValid_, now = playing && beatsValid;
    playing_ = playing;
    beatsValid_ = beatsValid;
    // MPC's position is this block's first sample; the control grid still has to add the samples
    // since its last step (advance()), so it starts that far back. Stopped: keep counting.
    if (now) beats_ = beats - static_cast<double>(sinceCtl_) * beatsPerSample_;
    if (now != was) {
        // EG Sync's count changes (the bar <-> from the note): on from where it is, no restart.
        for (int e = 0; e < 2; ++e) {
            const int s = (e ? patch_.aenv : patch_.fenv).sync;
            if (s > 0) envCycle_[e] = floorFast((now ? beats_ : envBeats_[e]) * kSyncPerBeat[std::min(s - 1, kNumSyncDivs - 1)]);
            envSkip_[e] = false;
        }
        // MPC stopped: a latched envelope with no key down lets go, as the keys have.
        if (was && !gate_) {
            fenv_.release();
            aenv_.release();
        }
    }
}

int Synth::activeVoices() const {
    if (silent_ || aenv_.stage == E_IDLE) return 0;
    return patch_.keyMode == KM_DUO && note2_ != note1_ ? 2 : 1;
}

Synth::Info Synth::info() const {
    return {gate_, note1_, note2_, glide_[0].pitch, glide_[1].pitch, fenv_.v, aenv_.v, nHeld_, silent_};
}

// --- modulation ---------------------------------------------------------------------------

// A bus's rate in Hz: free, Hi range or MPC's tempo, the other bus's Other Rate in it.
float Synth::busHz(const Bus& b, const ModPatch& m) const {
    if (!m.sync || m.hi) {   // Key Track: the rate follows the gliding key around C3
        const float kt = m.keyTrack != 0.0f ? exp2Fast(m.keyTrack * (glide_[0].pitch - kKeyCentre) * (1.0f / 12.0f)) : 1.0f;
        const float rate = m.rateHz > 1e-3f ? m.rateHz : 1e-3f;   // a hand-built Patch's 0 would stall Noise
        return std::min(rate * (m.hi ? kHiRange : 1.0f) * b.rateMul * kt, 0.25f * sr_);
    }
    return static_cast<float>(bpm_ * (1.0 / 60.0) * kSyncPerBeat[std::clamp(m.div, 0, kNumSyncDivs - 1)]) * b.rateMul;
}

void Synth::newCycle(Bus& b) {
    b.held = randBipolar(rng_);
    b.from = b.to;
    b.to = randBipolar(rng_);
}

// A source's value now: -1..1 for the LFO shapes (from the bus's phase), 0..1 for the envelopes
// and controllers, the key either way.
float Synth::sourceValue(const Bus& b, int src) const {
    const float ph = b.phase;
    switch (src) {
        case MS_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
        case MS_SAW: return 1.0f - 2.0f * ph;
        case MS_RAMP: return 2.0f * ph - 1.0f;
        case MS_SAMPLE_HOLD: return b.held;
        case MS_SMOOTH: return b.from + (b.to - b.from) * (0.5f - 0.5f * sinQuarter(kPi * (0.5f - ph)));   // cos(pi ph)
        case MS_FILTER_EG: return fenv_.v * fVel_;
        case MS_SINE: return sinCycle(ph);
        case MS_NOISE: return b.noise;
        case MS_AMP_EG: return aenv_.v * aVel_;
        case MS_VELOCITY: return vel_;
        case MS_AFTERTOUCH: return pressure_;
        case MS_KEY: return (glide_[0].pitch - kKeyCentre) * (1.0f / kKeySpan);
        case MS_CONSTANT: return 1.0f;
        case MS_BEND: return bend_;
        default: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);   // triangle
    }
}

// One bus's source over n samples: its value at the end of them. (A sounding Hi-range bus: n =
// 0, a read; renderRun moves it.)
float Synth::busValue(Bus& b, const ModPatch& m, int n) {
    const float hz = busHz(b, m);
    if (m.sync && !m.hi && !m.retrig && playing_ && beatsValid_ && !b.rateModulated) {   // locked to MPC's bar position
        double ph = beats_ * kSyncPerBeat[std::clamp(m.div, 0, kNumSyncDivs - 1)];
        ph -= floorFast(ph);
        if (static_cast<float>(ph) < b.phase) newCycle(b);
        b.phase = static_cast<float>(ph);
    } else {
        float next = b.phase + hz * static_cast<float>(n) * invSr_;
        if (next >= 1.0f) {
            next -= floorFast(next);
            newCycle(b);
        }
        b.phase = next;
    }
    if (m.src == MS_NOISE && n > 0) {   // a new random value every step, through the one-pole
        const float k = noisePole(hz, static_cast<float>(n), invSr_);
        b.lp += (randBipolar(rng_) - b.lp) * k;
        b.noise = clampf(b.lp * noiseGain(k), -1.0f, 1.0f);
    }
    return sourceValue(b, m.src);
}

// A slow random walk toward a new target every 0.3-1.5 s.
float Synth::driftStep(Drift& d, int n) {
    d.left -= n;
    if (d.left <= 0) {
        d.target = randBipolar(rng_);
        d.left = static_cast<int>(sr_ * (0.3f + 1.2f * static_cast<float>(xorshift(rng_) >> 8) / 16777216.0f));
    }
    d.v += (d.target - d.v) * std::min(1.0f, static_cast<float>(n) * driftK_);
    return d.v;
}

// Analog's faster drift: toward a new target every 40-200 ms, smoothed over 60 ms.
float Synth::fastStep(Drift& d, int n) {
    d.left -= n;
    if (d.left <= 0) {
        d.target = randBipolar(anaRng_);
        d.left = static_cast<int>(sr_ * (0.04f + 0.16f * static_cast<float>(xorshift(anaRng_) >> 8) / 16777216.0f));
    }
    d.v += (d.target - d.v) * std::min(1.0f, static_cast<float>(n) / (0.06f * sr_));
    return d.v;
}

// A bus's depth: its Control's, plus the wheel, velocity and pressure amounts, within -1..1.
float Synth::busDepth(const ModPatch& m) const {
    const float ctl = m.control == MC_MODWHEEL ? wheel_ : m.control == MC_AFTERTOUCH ? pressure_
                    : m.control == MC_VELOCITY ? vel_ : m.control == MC_NONE ? 0.0f : 1.0f;
    return clampf(ctl + m.wheel * wheel_ + m.vel * vel_ + m.at * pressure_, -1.0f, 1.0f);
}

// Glide Time and EG Time for a note starting now: control() works them out every step, but a note
// can't wait for the next one (its own velocity, the busses of a voice that was silent).
void Synth::noteTimeMods() {
    float glide = 0.0f, t[2] = {};
    bool any = false;
    for (int b = 0; b < 2; ++b) {
        const ModPatch& m = patch_.mod[b];
        if (m.amount == 0.0f || (m.dest != MD_GLIDE && m.dest != MD_EG_TIME && m.dest != MD_FEG_TIME && m.dest != MD_AEG_TIME))
            continue;
        // An LFO's value from the last catch-up; Key the key now played (not the one it glides from);
        // anything else (velocity, an envelope...) as it is.
        const float src = isLfo(m.src) ? busOut_[b]
                        : m.src == MS_KEY ? (static_cast<float>(note1_) - kKeyCentre) * (1.0f / kKeySpan)
                                          : sourceValue(bus_[b], m.src);
        const float out = src * busDepth(m);
        const float a = m.amount * out;
        if (m.dest == MD_GLIDE) glide += a;
        if (m.dest == MD_EG_TIME || m.dest == MD_FEG_TIME) t[0] += a;
        if (m.dest == MD_EG_TIME || m.dest == MD_AEG_TIME) t[1] += a;
        any = true;
    }
    if (!any && glideMul_ == 1.0f && egTimeMul_[0] == 1.0f && egTimeMul_[1] == 1.0f) return;
    glideMul_ = glide == 0.0f ? 1.0f : exp2Fast(kModTimeOctaves * clampf(glide, -1.0f, 1.0f));
    for (int e = 0; e < 2; ++e) egTimeMul_[e] = t[e] == 0.0f ? 1.0f : exp2Fast(kModTimeOctaves * clampf(t[e], -1.0f, 1.0f));
}

// Every kControl samples: glide, the mod busses, drift, and the targets every control value
// glides to over the next kControl samples.
// Time passes for everything that moves at the control rate: the song position, glides, the
// busses' sources (their values kept in busOut_), drift. n may be 0 (only the values are read).
// EG Sync: a held (or latched) envelope restarts at every sync unit, of MPC's bar while it plays,
// else counted from the note.
void Synth::envSync(int n) {
    for (int e = 0; e < 2; ++e) {
        const EnvPatch& ep = e ? patch_.aenv : patch_.fenv;
        if (ep.sync <= 0) continue;
        envBeats_[e] += static_cast<double>(n) * beatsPerSample_;
        Env& env = e ? aenv_ : fenv_;
        if (env.stage == E_IDLE || (env.stage == E_RELEASE && !env.looping) || !(gate_ || ep.latch)) continue;
        const double cycle = floorFast((playing_ && beatsValid_ ? beats_ : envBeats_[e]) *
                                       kSyncPerBeat[std::min(ep.sync - 1, kNumSyncDivs - 1)]);
        if (cycle != envCycle_[e]) {
            envCycle_[e] = cycle;
            if (envSkip_[e]) envSkip_[e] = false;   // the note's own unit
            else env.trigger(e ? ac_ : fc_, ep.reset);
        }
    }
}

void Synth::advance(int n) {
    beats_ += static_cast<double>(n) * beatsPerSample_;
    envSync(n);
    stepGlide(glide_[0], n);
    stepGlide(glide_[1], n);
    fVel_ = 1.0f - patch_.fenv.vel + patch_.fenv.vel * vel_;
    aVel_ = 1.0f - patch_.aenv.vel + patch_.aenv.vel * vel_;
    // A Hi-range bus moves in renderRun while the voice sounds, here while it is silent.
    for (int b = 0; b < 2; ++b) busOut_[b] = busValue(bus_[b], patch_.mod[b], fast_[b] && !silent_ ? 0 : n);
    for (int k = 0; k < 3; ++k) driftNow_[k] = driftStep(drift_[k], n);
    for (int k = 0; k < 3; ++k) fastNow_[k] = fastStep(fastDrift_[k], n);
}

// A note event between control steps: bring the control-rate time up to this sample first, so a
// new glide or a retriggered bus starts here, not some samples into the next step.
void Synth::catchUp() {
    // Silent: the oscillators run free on the control grid too (their phase summed the same
    // way whatever the block sizes; the ramps have arrived).
    if (silent_)
        for (int o = 0; o < 2; ++o) {
            const float d = dt_[o].v * static_cast<float>(kOversample * sinceCtl_);
            osc_[o].t += d - floorFast(osc_[o].t + d);
        }
    advance(sinceCtl_);
    sinceCtl_ = 0;
}

void Synth::control() {
    catchUp();
    const Patch& p = patch_;

    float pitchMod[2] = {}, cutMod = 0.0f, waveMod[2] = {}, resMod = 0.0f, driveMod = 0.0f, lvlMod[5] = {};
    float volMul = 1.0f, otherRate[2] = {1.0f, 1.0f}, egAmtMod = 0.0f, kbMod = 0.0f, beatMod = 0.0f;
    float timeMod[2] = {}, glideMod = 0.0f;
    for (int b = 0; b < 2; ++b) {
        const ModPatch& m = p.mod[b];
        const float depth = busDepth(m);
        const float out = busOut_[b] * depth;
        const float semis = m.pitch * std::fabs(m.pitch) * kModPitchRange;   // per unit of the source
        const float cut = m.filter * std::fabs(m.filter) * kModCutoffRange;
        // A Hi-range bus moves pitch, cutoff, wave and volume every sample (renderRun); here only
        // how far, with its depth. Its other destinations take it at the control rate.
        const bool fast = fast_[b];
        FastMod& f = fm_[b];
        f = FastMod{};
        if (fast) {
            f.pitch[0] = m.pitchDest != OD_OSC2 ? semis * depth : 0.0f;
            f.pitch[1] = m.pitchDest != OD_OSC1 ? semis * depth : 0.0f;
            f.cut = cut * depth;
        } else {
            if (m.pitchDest != OD_OSC2) pitchMod[0] += semis * out;
            if (m.pitchDest != OD_OSC1) pitchMod[1] += semis * out;
            cutMod += cut * out;
        }
        const float a = m.amount * out, af = m.amount * depth;
        switch (m.dest) {
            case MD_WAVE:
                if (fast) {
                    f.wave[0] = f.wave[1] = af;
                } else {
                    waveMod[0] += a;
                    waveMod[1] += a;
                }
                break;
            case MD_WAVE1:
                if (fast) f.wave[0] = af;
                else waveMod[0] += a;
                break;
            case MD_WAVE2:
                if (fast) f.wave[1] = af;
                else waveMod[1] += a;
                break;
            case MD_VOLUME:
                if (fast) f.vol = af;
                else volMul *= std::max(0.0f, 1.0f + a);
                break;
            case MD_RES: resMod += a; break;
            case MD_DRIVE: driveMod += a; break;
            case MD_OSC1: lvlMod[0] += a; break;
            case MD_SUB: lvlMod[1] += a; break;
            case MD_OSC2: lvlMod[2] += a; break;
            case MD_NOISE: lvlMod[3] += a; break;
            case MD_FEEDBACK: lvlMod[4] += a; break;
            case MD_OTHER_RATE: otherRate[1 - b] = exp2Fast(clampf(a, -1.0f, 1.0f) * kOtherRateOctaves); break;
            case MD_EG_AMOUNT: egAmtMod += a; break;
            case MD_KEY_TRACK: kbMod += a; break;
            case MD_BEAT: beatMod += a; break;
            case MD_EG_TIME:
                timeMod[0] += a;
                timeMod[1] += a;
                break;
            case MD_FEG_TIME: timeMod[0] += a; break;
            case MD_AEG_TIME: timeMod[1] += a; break;
            case MD_GLIDE: glideMod += a; break;
            default: break;
        }
    }
    bus_[0].rateMul = otherRate[0];
    bus_[1].rateMul = otherRate[1];
    // The times, like every programmable destination, within their range (x1/8..x8) however many
    // busses and whatever the source (Key reaches 2.8 at the keyboard's ends).
    glideMul_ = glideMod == 0.0f ? 1.0f : exp2Fast(kModTimeOctaves * clampf(glideMod, -1.0f, 1.0f));
    // EG Time: new coefficients once the times have moved (by 0.2%, or back to where they were),
    // without libm (this can happen every step).
    bool retime = false;
    for (int e = 0; e < 2; ++e) {
        const float mul = timeMod[e] == 0.0f ? 1.0f : exp2Fast(kModTimeOctaves * clampf(timeMod[e], -1.0f, 1.0f));
        if (mul != egTimeMul_[e] && (mul == 1.0f || std::fabs(mul - egTimeMul_[e]) > 0.002f * egTimeMul_[e])) {
            egTimeMul_[e] = mul;
            retime = true;
        }
    }
    if (retime) envCoefs(true);

    // Drift: the slow wander, the note's offset; Analog's faster wander and the unit's tuning.
    float driftCents[2] = {driftNow_[0] * p.drift * 6.0f + noteDrift_[0] * p.drift * 3.0f,
                           driftNow_[1] * p.drift * 6.0f + noteDrift_[1] * p.drift * 3.0f};
    float driftCut = driftNow_[2] * p.drift * 0.6f;
    float noteWave[2] = {}, noteRes = 0.0f;
    if (an_.on) {   // only then: at 0 the sums are exactly what they were before Analog
        driftCents[0] += fastNow_[0] * p.drift * kAnaFast + an_.cents[0];
        driftCents[1] += fastNow_[1] * p.drift * kAnaFast + an_.cents[1];
        driftCut += fastNow_[2] * p.drift * kAnaFastCut + noteCut_ * p.drift * kAnaNoteCut + an_.cut;
        noteWave[0] = noteWave_[0] * p.drift * kAnaNoteWave;
        noteWave[1] = noteWave_[1] * p.drift * kAnaNoteWave;
        noteRes = noteRes_ * p.drift * kAnaNoteRes + an_.res;
    }
    const float bend = bend_ * (bend_ > 0.0f ? p.bendUp : p.bendDown);
    const float bend1 = p.bendDest == BD_BOTH || p.bendDest == BD_OSC1 ? bend : 0.0f;
    const float bend2 = p.bendDest == BD_BOTH || p.bendDest == BD_OSC2 ? bend : 0.0f;
    // A droning osc 2 follows no key: C3 at 8', its frequency knob reaching +-3 octaves.
    const bool drone = p.osc2Keys == O2_DRONE;
    const float key2 = drone ? kDroneNote + p.osc2Semis * (kDroneSpan / 7.0f) : glide_[1].pitch + p.osc2Semis;
    const float pitch[2] = {
        glide_[0].pitch + 12.0f * static_cast<float>(p.osc[0].octave) + bend1 + pitchMod[0] + 0.01f * driftCents[0],
        key2 + 12.0f * static_cast<float>(p.osc[1].octave) + bend2 + pitchMod[1] + 0.01f * driftCents[1]};
    const float drive = clampf(p.drive + driveMod, 0.0f, 1.0f);
    const float driveGain = 1.0f + kDriveSpan * drive * drive;
    const float sd = sinCycle(0.5f * drive);   // sin(pi drive)
    driveBias_ = kDriveBias * sd * sd;          // no ramp: it only shapes the clipper
    const float aVel = 1.0f - p.aenv.vel + p.aenv.vel * vel_;

    const float target[] = {
        clampf(noteHz(pitch[0]) * invOsr_, 1e-7f, 0.45f),
        clampf((noteHz(pitch[1]) + p.beatHz + kBeatRange * clampf(beatMod, -1.0f, 1.0f)) * invOsr_, 1e-7f, 0.45f),   // Beat Freq: Hz
        clampf(p.osc[0].wave + waveMod[0] + noteWave[0], 0.0f, 1.0f),
        clampf(p.osc[1].wave + waveMod[1] + noteWave[1], 0.0f, 1.0f),
        cutNote_ + clampf(p.keyTrack + 2.0f * kbMod, 0.0f, 2.0f) * (glide_[0].pitch - kKeyCentre) + cutMod + driftCut,
        envSemis(clampf(p.envAmount + egAmtMod, -1.0f, 1.0f)) * fVel_,
        resFeedback(clampf(p.res + resMod + noteRes, 0.0f, 1.0f)),
        kInGain * driveGain,
        kPostBase + kPostSpan * drive,           // Multidrive's second stage: into the clipper...
        1.0f / (kPostBase + kPostSpan * drive),  // ...and out of it
        taper(p.mixOsc1 + lvlMod[0]),
        taper(p.mixSub + lvlMod[1]),
        taper(p.mixOsc2 + lvlMod[2]),
        taper(p.mixNoise + lvlMod[3]),
        taper(p.mixFeedback + lvlMod[4]) * kFeedbackGain,
        kOutGain * aVel * volMul * exp2Fast(-0.35f * log2Fast(driveGain)),   // / driveGain^0.35
        exp2Fast(clampf(p.volumeDb, -60.0f, 12.0f) * (1.0f / 6.0206f)) * (p.volumeDb <= -59.5f ? 0.0f : 1.0f),
    };
    const auto rs = ramps();
    static_assert(sizeof target / sizeof target[0] == std::tuple_size<decltype(rs)>::value, "a target per ramp");
    constexpr float invN = 1.0f / static_cast<float>(kControl);
    for (size_t i = 0; i < rs.size(); ++i) {
        // A pitch jump (a new note without glide) lands at once; everything else glides. The
        // very first step starts every value where it belongs.
        if (snapAll_ || (snap_ && i < 2)) rs[i]->snap(target[i]);
        else rs[i]->to(target[i], invN);
    }
    // Another slope mid-note: crossfade the ladder's taps instead of switching.
    const int slope = std::clamp(p.slope, 0, 3);
    if (slope != tap_) {
        tapFrom_ = snapAll_ ? slope : tap_;
        tap_ = slope;
        xfLeft_ = snapAll_ ? 0 : kTapFade;
    }
    snap_ = snapAll_ = false;

    // Idle: once the amp envelope is done and the output has died away, stop rendering.
    if (!silent_) {
        if (aenv_.stage == E_IDLE && peak_ < 1e-5f) {
            if (++idleSteps_ >= 4) goSilent();
        } else {
            idleSteps_ = 0;
        }
    }
    peak_ = 0.0f;
}

void Synth::goSilent() {
    silent_ = true;
    idleSteps_ = 0;
    ladder_.reset();
    dec_.reset();
    fbIn_ = fbX1_ = fbY1_ = fbLp_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    noiseLp_ = noiseHp_ = pink_[0] = pink_[1] = pink_[2] = 0.0f;
    oscLp_ = hpX_ = hpY_ = 0.0f;
    aePrev_ = 0.0f;
}

// --- audio --------------------------------------------------------------------------------

void Synth::render(float* outL, float* outR, int n) {
    StageClock clock;
    int pos = 0;
    while (pos < n) {
        if (ctlLeft_ <= 0) {
            // Silent: only time moves on (glides, busses, drift), on the same grid, so block sizes
            // never change the sound; the values are worked out when a note wakes the voice.
            if (silent_) catchUp();
            else control();
            ctlLeft_ = kControl;
            clock.lap(STG_CONTROL);
        }
        const int m = std::min(n - pos, ctlLeft_);
        if (silent_) {
            renderSilent(outL + pos, m);
            clock.lap(STG_IDLE);
        } else {
            renderRun(outL + pos, m);
            clock.lap(STG_VOICE);
        }
        pos += m;
        ctlLeft_ -= m;
        sinceCtl_ += m;
    }
    for (int i = 0; i < n; ++i) outR[i] = outL[i];
}

// Nothing sounds: the filter envelope keeps its release, the control values arrive where they
// were going (and snap to new ones on waking); the oscillators run free in catchUp().
void Synth::renderSilent(float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = 0.0f;
    for (Ramp* r : ramps()) r->arrive();
    if (fenv_.stage != E_IDLE)
        for (int i = 0; i < n; ++i) fenv_.tick(fc_);
}

// The Hi-range busses over a run of n base samples: their phases move every sample, and what
// they do to pitch (as frequency multipliers), cutoff (semitones), wave and volume.
void Synth::fastMods(int n, FastRun& r) {
    float semis[2][kControl];
    for (int i = 0; i < kControl; ++i) {
        semis[0][i] = semis[1][i] = 0.0f;
        r.cut[i] = r.wave[0][i] = r.wave[1][i] = 0.0f;
        r.vol[i] = 1.0f;
    }
    float inc[2] = {}, k[2] = {}, g[2] = {};
    for (int b = 0; b < 2; ++b)
        if (fast_[b]) {
            const float hz = busHz(bus_[b], patch_.mod[b]);
            inc[b] = hz * invSr_;   // busHz keeps it under 0.25
            if (patch_.mod[b].src == MS_NOISE) {
                k[b] = noisePole(hz, 1.0f, invSr_);
                g[b] = noiseGain(k[b]);
            }
        }
    // Sample by sample, both busses in turn: the random values they draw come in the same order
    // however a block splits the run.
    for (int i = 0; i < n; ++i)
        for (int b = 0; b < 2; ++b) {
            if (!fast_[b]) continue;
            Bus& x = bus_[b];
            const int src = patch_.mod[b].src;
            const FastMod& f = fm_[b];
            float ph = x.phase + inc[b];
            if (ph >= 1.0f) {
                ph -= floorFast(ph);
                newCycle(x);
            }
            x.phase = ph;
            if (src == MS_NOISE) {
                x.lp += (randBipolar(rng_) - x.lp) * k[b];
                x.noise = clampf(x.lp * g[b], -1.0f, 1.0f);
            }
            const float v = sourceValue(x, src);
            semis[0][i] += f.pitch[0] * v;
            semis[1][i] += f.pitch[1] * v;
            r.cut[i] += f.cut * v;
            r.wave[0][i] += f.wave[0] * v;
            r.wave[1][i] += f.wave[1] * v;
            r.vol[i] *= std::max(0.0f, 1.0f + f.vol * v);
        }
    for (int o = 0; o < 2; ++o)
        for (int i = 0; i < kControl; i += 4) store4(r.pm[o] + i, exp2Fast4(load4(semis[o] + i) * splat(1.0f / 12.0f)));
}

// n <= kControl samples of the voice, in two passes. First, per base sample: the envelopes, the
// cutoff they move and its coefficient (four at a time: dsp/simd.h), the VCA. Then the audio, at
// the 2x rate, with the coefficients ready.
void Synth::renderRun(float* out, int n) {
    // [0]: the previous base sample's, for the first half-step; [1..n]: this run's.
    float fq[kControl + 1], ae[kControl + 1], cs[kControl];
    const bool fast = fast_[0] || fast_[1];   // a Hi-range bus: its offsets for every sample of the run
    FastRun fr;
    if (fast) fastMods(n, fr);
    for (int i = 0; i < n; ++i) {
        const float fe = fenv_.tick(fc_);
        ae[i + 1] = aenv_.tick(ac_);
        cs[i] = cut_.next() + egAmt_.next() * fe + (fast ? fr.cut[i] : 0.0f);
    }
    for (int i = n; i < kControl; ++i) cs[i] = cs[n - 1];   // the last vector's spare lanes
    {
        const f4 lo = splat(kMinCutoff), hi = splat(kMaxCutoff * osr_), w = splat(kPi * invOsr_);
        for (int i = 0; i < n; i += 4) {   // tan(pi fc / 2fs), fc = 440 * 2^((note - 69) / 12) clamped
            const f4 hz = splat(440.0f) * exp2Fast4((load4(cs + i) - splat(69.0f)) * splat(1.0f / 12.0f));
            float f[4];
            store4(f, tanFast4(min4(max4(hz, lo), hi) * w));
            for (int k = 0; k < 4 && i + k < n; ++k) fq[1 + i + k] = f[k];
        }
    }
    if (!fPrevValid_) {   // just woken: no stale cutoff from before the silence
        fPrev_ = fq[1];
        fPrevValid_ = true;
    }
    fq[0] = fPrev_;
    ae[0] = aePrev_;

    const int tap = tap_, tapFrom = tapFrom_;
    const int subOct = patch_.subOctave == SO_TWO ? 2 : 1;
    const bool sync = patch_.sync;
    const float nk = noiseK_, np = noisePink_, nc = noiseComp_;
    const float fbR = 1.0f - 2.0f * kPi * kFeedbackHp * invOsr_;   // the feedback loop's AC coupling
    // Multidrive's second stage: softclip(g x + b) - softclip(b), tube-like (even harmonics) at
    // moderate drive, toward symmetric hard clipping at full.
    const float bias = driveBias_, biasOut = softclip(bias);
    // Noise and feedback only cost when they are up (their level ramps sit at exactly 0 otherwise).
    const bool noiseOn = lvl_[3].v != 0.0f || lvl_[3].d != 0.0f;
    const bool fbOn = lvl_[4].v != 0.0f || lvl_[4].d != 0.0f;
    if (!fbOn) fbIn_ = fbLp_ = fbX1_ = fbY1_ = 0.0f;   // off: it starts clean when it comes up
    const float dcR = 1.0f - 2.0f * kPi * 5.0f * invSr_;       // output DC blocker, 5 Hz
    // A still wave knob (no sweep, no bus on it): its shape once, not every sample.
    const bool fw1 = fast && (fm_[0].wave[0] != 0.0f || fm_[1].wave[0] != 0.0f);
    const bool fw2 = fast && (fm_[0].wave[1] != 0.0f || fm_[1].wave[1] != 0.0f);
    const bool morph1 = wave_[0].d != 0.0f || fw1, morph2 = wave_[1].d != 0.0f || fw2;
    const Analog an = an_;   // a copy in registers: the per-sample loop reads it
    Shape s1 = shapeOf(wave_[0].v, an.pwOff[0], an.bow[0]), s2 = shapeOf(wave_[1].v, an.pwOff[1], an.bow[1]);
    float peak = peak_;
    for (int i = 0; i < n; ++i) {
        float dt1 = dt_[0].next(), dt2 = dt_[1].next(), vm = 1.0f;
        if (fast) {
            dt1 = std::min(dt1 * fr.pm[0][i], 0.45f);
            dt2 = std::min(dt2 * fr.pm[1][i], 0.45f);
            vm = fr.vol[i];
        }
        if (an.on) {   // this cycle's length (jitter)
            dt1 *= jit_[0];
            dt2 *= jit_[1];
        }
        if (morph1) s1 = shapeOf(wave_[0].next() + (fw1 ? fr.wave[0][i] : 0.0f), an.pwOff[0], an.bow[0]);
        if (morph2) s2 = shapeOf(wave_[1].next() + (fw2 ? fr.wave[1][i] : 0.0f), an.pwOff[1], an.bow[1]);
        const float l0 = lvl_[0].next(), l1 = lvl_[1].next(), l2 = lvl_[2].next(), l3 = lvl_[3].next() * nc,
                    l4 = lvl_[4].next();
        const float r = res_.next(), gain = inGain_.next(), postIn = postIn_.next(), postOut = postOut_.next(),
                    vca = vca_.next() * vm;
        const bool fade = xfLeft_ > 0;   // a slope change, crossfading the taps
        const float xf = fade ? static_cast<float>(xfLeft_--) * (1.0f / kTapFade) : 0.0f;   // of the old tap
        float hi[kOversample];
        for (int k = 0; k < kOversample; ++k) {
            // Cutoff and VCA move per base sample; the first half-step goes halfway.
            const float fk = k == 0 ? 0.5f * (fq[i] + fq[i + 1]) : fq[i + 1];
            const float amp = (k == 0 ? 0.5f * (ae[i] + ae[i + 1]) : ae[i + 1]) * vca;
            float v1, v2, vs;
            bool w1 = false, w2 = false;
            if (resetPending_) {   // keyboard reset: every oscillator restarts its cycle here
                v1 = osc_[0].tickReset(dt1, s1, 1.0f);
                v2 = osc_[1].tickReset(dt2, s2, 1.0f);
                sub_.reset(subOct);
                vs = sub_.tick(false, 0.0f, subOct);
                resetPending_ = false;
            } else {
                float x1 = 0.0f, x2 = 0.0f;
                v1 = osc_[0].tick(dt1, s1, w1, x1);
                v2 = sync && w1 ? osc_[1].tickReset(dt2, s2, x1) : osc_[1].tick(dt2, s2, w2, x2);
                vs = sub_.tick(w1, x1, subOct);
            }
            if (an.on) {   // a new cycle, a slightly different length
                if (w1) jit_[0] = 1.0f + an.jitter * randBipolar(jitRng_);
                if (w2) jit_[1] = 1.0f + an.jitter * randBipolar(jitRng_);
            }
            const float white = randBipolar(noiseRng_);
            float mix = l0 * v1 + l1 * vs + l2 * v2;
            if (an.on) {
                // The oscillators' bandwidth (soft edges and tips) and their coupling into the mixer
                // (a square sags at bass notes): one stage for the three, the same as one each.
                oscLp_ += (mix - oscLp_) * an.lpK;
                hpY_ = oscLp_ - hpX_ + an.hpR * hpY_;
                hpX_ = oscLp_;
                mix = hpY_;
            }
            if (noiseOn) {
                pink_[0] = kPinkPole[0] * pink_[0] + kPinkGain[0] * white;
                pink_[1] = kPinkPole[1] * pink_[1] + kPinkGain[1] * white;
                pink_[2] = kPinkPole[2] * pink_[2] + kPinkGain[2] * white;
                const float pink = pink_[0] + pink_[1] + pink_[2] + kPinkDirect * white;
                const float src = white + (pink - white) * np;
                noiseHp_ += (src - noiseHp_) * kNoiseHp;
                noiseLp_ += (src - noiseHp_ - noiseLp_) * nk;
                mix += l3 * noiseLp_;
            }
            // The mixer, its feedback channel taking the mixer's own output back in (one sample
            // late): through that channel's overload (a cubic: no division in the loop), AC
            // coupled and band-limited, as the original's FEEDBACK knob does with nothing in EXT IN.
            // Over unity loop gain it saturates: grit, then the howl of an overdriven loop.
            if (fbOn) {
                mix += l4 * fbIn_;
                // c - c^3 / 3: slope 1 at 0, flat at +-1, where it reads 2/3 of the scale: kFeedbackClip.
                const float c = clampf(mix * (1.0f / (1.5f * kFeedbackClip)), -1.0f, 1.0f);
                fbLp_ += (1.5f * kFeedbackClip * c * (1.0f - (1.0f / 3.0f) * c * c) - fbLp_) * fbK_;
                fbY1_ = fbLp_ - fbX1_ + fbR * fbY1_;
                fbX1_ = fbLp_;
                fbIn_ = fbY1_;
            }
            float y[4];
            // The circuit's own noise floor (-80 dB): what starts a self-oscillating filter with
            // every source down, as on the hardware.
            ladder_.tick(mix * gain + kThermal * white, fk, r, y);
            const float yt = fade ? y[tap] + (y[tapFrom] - y[tap]) * xf : y[tap];
            const float v = (softclip(yt * postIn + bias) - biasOut) * postOut * amp;   // Multidrive's second stage, VCA
            hi[k] = v;
        }
        const float lo = dec_.process(hi[0], hi[1]);
        dcY1_ = lo - dcX1_ + dcR * dcY1_;
        dcX1_ = lo;
        const float o = dcY1_ * vol_.next();
        peak = std::max(peak, std::fabs(o));
        out[i] = o;
    }
    fPrev_ = fq[n];
    aePrev_ = ae[n];
    peak_ = peak;
}

} // namespace sf
