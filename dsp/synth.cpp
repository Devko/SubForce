#include "synth.h"

#include "stages.h"

#include <algorithm>
#include <cmath>

namespace sf {

#ifdef SF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

namespace {

constexpr float kInGain = 0.5f;          // mixer -> ladder at Multidrive 0 (one oscillator at full: mild warmth)
constexpr float kDriveSpan = 7.0f;       // Multidrive 1: 8x that
// The feedback loop (the mixer's output back into it), tuned on renders of a saw through it: from
// a slight thickening (+1 dB at half the knob) and more drive into the filter, through grit, to the
// chaos of a loop over unity gain in the last tenth (+5 dB, the upper harmonics +15 dB).
constexpr float kFeedbackGain = 1.4f;    // the loop's gain at full: unity at 85% of the knob
constexpr float kFeedbackClip = 0.7f;    // where the loop's own stage (the EXT IN level amp) saturates
constexpr float kFeedbackHp = 150.0f;    // Hz: AC coupled: no bass builds up around the loop
constexpr float kFeedbackLp = 7000.0f;   // Hz: the loop's bandwidth, an analog stage's
constexpr float kDriveBias = 0.3f;       // Multidrive's asymmetry at its middle (tube-like); subtle low, none at the ends
constexpr float kOutGain = 1.51f;        // engine output at 0 dB volume (1.2 until 0.0.1: +2 dB, level with MPC's own instruments)
constexpr float kMaxCutoff = 0.40f;      // of the 2x rate (35 kHz): the ladder's coefficient stays sane
constexpr float kMinCutoff = 8.0f;       // Hz
constexpr int kTapFade = 32;             // samples to crossfade a slope change
constexpr float kThermal = 1e-4f;        // the ladder's input noise floor (-80 dB)

// Mixer knobs: an audio taper.
inline float taper(float k) {
    k = clampf(k, 0.0f, 1.0f);
    return k * k;
}

inline float noteOf(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

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

Synth::Synth(float sampleRate)
    : sr_(sampleRate), osr_(sampleRate * kOversample), invOsr_(1.0f / (sampleRate * kOversample)),
      invSr_(1.0f / sampleRate), driftK_(1.0f / (0.6f * sampleRate)),
      fbK_(1.0f - std::exp(-2.0f * kPi * kFeedbackLp / (sampleRate * kOversample))) {
    setTransport(120.0, 0.0, false, false);
    setPatch(Patch{});
}

void Synth::setPatch(const Patch& p) {
    const bool repick = havePatch_ && (p.keyMode != patch_.keyMode || p.priority != patch_.priority);
    patch_ = p;
    havePatch_ = true;
    cutNote_ = noteOf(p.cutoffHz);
    // A bus whose rate the other one modulates runs free even when synced (a locked phase can't
    // speed up and slow down).
    for (int b = 0; b < 2; ++b)
        bus_[b].rateModulated = p.mod[1 - b].dest == MD_OTHER_RATE && p.mod[1 - b].amount != 0.0f;
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
    fc_ = envCoef(p.fenv, sr_, kbScale(p.fenv.kb));
    ac_ = envCoef(p.aenv, sr_, kbScale(p.aenv.kb));
    // Noise colour: white crossfading to pink up to 0.5, then pink through a one-pole down to ~220
    // Hz; the same RMS at every colour (kNoiseComp).
    const float nc = clampf(p.noiseColor, 0.0f, 1.0f);
    noisePink_ = std::min(1.0f, 2.0f * nc);
    noiseK_ = nc > 0.5f ? exp2Fast(-6.0f * (2.0f * nc - 1.0f)) : 1.0f;
    const float at = nc * 16.0f;
    const int i0 = std::min(static_cast<int>(at), 15);
    noiseComp_ = kNoiseComp[i0] + (kNoiseComp[i0 + 1] - kNoiseComp[i0]) * (at - static_cast<float>(i0));
}

float Synth::kbScale(float kb) const { return exp2Fast(-clampf(kb, 0.0f, 1.0f) * static_cast<float>(note1_ - 60) / 12.0f); }

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
            fenv_.release();
            aenv_.release();
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
    glideTo(glide_[0], static_cast<float>(n1), glide && patch_.glideDest != OD_OSC2);
    glideTo(glide_[1], static_cast<float>(n2), glide && patch_.glideDest != OD_OSC1);
    gate_ = true;
    // Multi retriggers on a key struck, not on a release that hands the oscillators back to a
    // key still held (a trill would attack twice, a Duo pair's other key on every lift).
    if (fresh || (patch_.trigger == TR_MULTI && pressed >= 0)) trigger(vel);
    ctlLeft_ = 0;   // the new pitch from the next sample on
}

void Synth::trigger(int vel) {
    vel_ = static_cast<float>(std::clamp(vel, 1, 127)) / 127.0f;
    fc_ = envCoef(patch_.fenv, sr_, kbScale(patch_.fenv.kb));
    ac_ = envCoef(patch_.aenv, sr_, kbScale(patch_.aenv.kb));
    fenv_.trigger(fc_, patch_.fenv.reset);
    aenv_.trigger(ac_, patch_.aenv.reset);
    for (int b = 0; b < 2; ++b)
        if (patch_.mod[b].retrig) {
            Bus& x = bus_[b];
            x.phase = 0.0f;
            x.held = randBipolar(rng_);
            x.from = x.to;
            x.to = randBipolar(rng_);
        }
    if (patch_.kbReset) resetPending_ = true;
    for (float& d : noteDrift_) d = randBipolar(rng_) * patch_.drift * 3.0f;   // cents
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
    if (!glide || dist < 1e-4f || patch_.glideTime < 1e-4f) {
        g.pitch = target;
        g.left = 0;
        snap_ = true;
        return;
    }
    if (patch_.glideType == GT_EXP) {   // RC: 99% of the way at glideTime (e^-4.6)
        g.exp = true;
        g.lk = -4.6f / (patch_.glideTime * sr_ * 0.693147181f);
        g.left = 1;
        return;
    }
    // Counted in samples, not summed steps: a slow, small glide never stalls on rounding.
    const float samples = patch_.glideTime * sr_ * (patch_.glideType == GT_RATE ? dist / 12.0f : 1.0f);
    g.exp = false;
    g.from = g.pitch;
    g.len = g.left = static_cast<int>(std::clamp(samples, 1.0f, 1e9f));
}

void Synth::stepGlide(Glide& g, int n) const {
    if (g.left <= 0) return;
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
        fenv_.release();
        aenv_.release();
    }
}

void Synth::allNotesOff() {
    nHeld_ = 0;
    if (gate_) {
        gate_ = false;
        fenv_.release();
        aenv_.release();
    }
}

void Synth::reset() {
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
}

void Synth::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
    beatsPerSample_ = bpm_ / 60.0 / static_cast<double>(sr_);
    playing_ = playing;
    beatsValid_ = beatsValid;
    if (playing && beatsValid) beats_ = beats;   // stopped: keep counting on our own
}

int Synth::activeVoices() const {
    if (silent_ || aenv_.stage == E_IDLE) return 0;
    return patch_.keyMode == KM_DUO && note2_ != note1_ ? 2 : 1;
}

Synth::Info Synth::info() const {
    return {gate_, note1_, note2_, glide_[0].pitch, glide_[1].pitch, fenv_.v, aenv_.v, nHeld_, silent_};
}

// --- modulation ---------------------------------------------------------------------------

// One bus's source over n samples: its value at the end of them (-1..1; the filter envelope 0..1).
float Synth::busValue(Bus& b, const ModPatch& m, int n) {
    const double perBeat = kSyncPerBeat[std::clamp(m.div, 0, kNumSyncDivs - 1)];   // cycles
    auto newCycle = [&] {
        b.held = randBipolar(rng_);
        b.from = b.to;
        b.to = randBipolar(rng_);
    };
    if (m.sync && !m.retrig && playing_ && beatsValid_ && !b.rateModulated) {   // locked to MPC's bar position
        double ph = beats_ * perBeat;
        ph -= floorFast(ph);
        if (static_cast<float>(ph) < b.phase) newCycle();
        b.phase = static_cast<float>(ph);
    } else {
        const float hz = (m.sync ? static_cast<float>(bpm_ * (1.0 / 60.0) * perBeat) : m.rateHz) * b.rateMul;
        float next = b.phase + hz * static_cast<float>(n) * invSr_;
        if (next >= 1.0f) {
            next -= floorFast(next);
            newCycle();
        }
        b.phase = next;
    }
    const float ph = b.phase;
    switch (m.src) {
        case MS_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
        case MS_SAW: return 1.0f - 2.0f * ph;
        case MS_RAMP: return 2.0f * ph - 1.0f;
        case MS_SAMPLE_HOLD: return b.held;
        case MS_SMOOTH: return b.from + (b.to - b.from) * (0.5f - 0.5f * sinQuarter(kPi * (0.5f - ph)));   // cos(pi ph)
        case MS_FILTER_EG: return fenv_.v * fVel_;
        default: return ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f);   // triangle
    }
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

// Every kControl samples: glide, the mod busses, drift, and the targets every control value
// glides to over the next kControl samples.
// Time passes for everything that moves at the control rate: the song position, glides, the
// busses' sources (their values kept in busOut_), drift. n may be 0 (only the values are read).
void Synth::advance(int n) {
    beats_ += static_cast<double>(n) * beatsPerSample_;
    stepGlide(glide_[0], n);
    stepGlide(glide_[1], n);
    fVel_ = 1.0f - patch_.fenv.vel + patch_.fenv.vel * vel_;
    for (int b = 0; b < 2; ++b) busOut_[b] = busValue(bus_[b], patch_.mod[b], n);
    for (int k = 0; k < 3; ++k) driftNow_[k] = driftStep(drift_[k], n);
}

// A note event between control steps: bring the control-rate time up to this sample first, so a
// new glide or a retriggered bus starts here, not some samples into the next step.
void Synth::catchUp() {
    advance(sinceCtl_);
    sinceCtl_ = 0;
}

void Synth::control() {
    catchUp();
    const Patch& p = patch_;

    float pitchMod[2] = {}, cutMod = 0.0f, waveMod[2] = {}, resMod = 0.0f, driveMod = 0.0f, lvlMod[5] = {};
    float volMul = 1.0f, otherRate[2] = {1.0f, 1.0f};
    for (int b = 0; b < 2; ++b) {
        const ModPatch& m = p.mod[b];
        const float depth = m.control == MC_MODWHEEL ? wheel_ : m.control == MC_AFTERTOUCH ? pressure_
                          : m.control == MC_VELOCITY ? vel_ : 1.0f;
        const float out = busOut_[b] * depth;
        const float semis = m.pitch * std::fabs(m.pitch) * kModPitchRange * out;
        if (m.pitchDest != OD_OSC2) pitchMod[0] += semis;
        if (m.pitchDest != OD_OSC1) pitchMod[1] += semis;
        cutMod += m.filter * std::fabs(m.filter) * kModCutoffRange * out;
        const float a = m.amount * out;
        switch (m.dest) {
            case MD_WAVE: waveMod[0] += a; waveMod[1] += a; break;
            case MD_WAVE1: waveMod[0] += a; break;
            case MD_WAVE2: waveMod[1] += a; break;
            case MD_RES: resMod += a; break;
            case MD_DRIVE: driveMod += a; break;
            case MD_SUB: lvlMod[1] += a; break;
            case MD_NOISE: lvlMod[3] += a; break;
            case MD_FEEDBACK: lvlMod[4] += a; break;
            case MD_VOLUME: volMul *= std::max(0.0f, 1.0f + a); break;
            case MD_OTHER_RATE: otherRate[1 - b] = exp2Fast(a * kOtherRateOctaves); break;
            default: break;
        }
    }
    bus_[0].rateMul = otherRate[0];
    bus_[1].rateMul = otherRate[1];

    const float driftCents[2] = {driftNow_[0] * p.drift * 6.0f + noteDrift_[0], driftNow_[1] * p.drift * 6.0f + noteDrift_[1]};
    const float driftCut = driftNow_[2] * p.drift * 0.6f;
    const float bend = bend_ * (bend_ > 0.0f ? p.bendUp : p.bendDown);
    const float pitch[2] = {
        glide_[0].pitch + 12.0f * static_cast<float>(p.osc[0].octave) + bend + pitchMod[0] + 0.01f * driftCents[0],
        glide_[1].pitch + 12.0f * static_cast<float>(p.osc[1].octave) + p.osc2Semis + bend + pitchMod[1] + 0.01f * driftCents[1]};
    const float drive = clampf(p.drive + driveMod, 0.0f, 1.0f);
    const float driveGain = 1.0f + kDriveSpan * drive * drive;
    const float sd = sinCycle(0.5f * drive);   // sin(pi drive)
    driveBias_ = kDriveBias * sd * sd;          // no ramp: it only shapes the clipper
    const float aVel = 1.0f - p.aenv.vel + p.aenv.vel * vel_;

    const float target[] = {
        clampf(noteHz(pitch[0]) * invOsr_, 1e-7f, 0.45f),
        clampf(noteHz(pitch[1]) * invOsr_, 1e-7f, 0.45f),
        clampf(p.osc[0].wave + waveMod[0], 0.0f, 1.0f),
        clampf(p.osc[1].wave + waveMod[1], 0.0f, 1.0f),
        cutNote_ + p.keyTrack * (glide_[0].pitch - 60.0f) + cutMod + driftCut,
        envSemis(p.envAmount) * fVel_,
        resFeedback(clampf(p.res + resMod, 0.0f, 1.0f)),
        kInGain * driveGain,
        0.5f + drive,                  // Multidrive's second stage: into the clipper...
        2.0f / (1.0f + 2.0f * drive),  // ...and out of it
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

// Nothing sounds: the oscillators keep running (free phase), the filter envelope keeps its
// release, the control values arrive where they were going (and snap to new ones on waking).
void Synth::renderSilent(float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = 0.0f;
    for (int o = 0; o < 2; ++o) {
        const float d = dt_[o].v * static_cast<float>(kOversample * n);
        osc_[o].t += d - floorFast(osc_[o].t + d);
    }
    for (Ramp* r : ramps()) r->arrive();
    if (fenv_.stage != E_IDLE)
        for (int i = 0; i < n; ++i) fenv_.tick(fc_);
}

// n <= kControl samples of the voice, in two passes. First, per base sample: the envelopes, the
// cutoff they move and its coefficient (four at a time: dsp/simd.h), the VCA. Then the audio, at
// the 2x rate, with the coefficients ready.
void Synth::renderRun(float* out, int n) {
    // [0]: the previous base sample's, for the first half-step; [1..n]: this run's.
    float fq[kControl + 1], ae[kControl + 1], cs[kControl];
    for (int i = 0; i < n; ++i) {
        const float fe = fenv_.tick(fc_);
        ae[i + 1] = aenv_.tick(ac_);
        cs[i] = cut_.next() + egAmt_.next() * fe;
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
    const bool morph1 = wave_[0].d != 0.0f, morph2 = wave_[1].d != 0.0f;
    Shape s1 = shapeOf(wave_[0].v), s2 = shapeOf(wave_[1].v);
    float peak = peak_;
    for (int i = 0; i < n; ++i) {
        const float dt1 = dt_[0].next(), dt2 = dt_[1].next();
        if (morph1) s1 = shapeOf(wave_[0].next());
        if (morph2) s2 = shapeOf(wave_[1].next());
        const float l0 = lvl_[0].next(), l1 = lvl_[1].next(), l2 = lvl_[2].next(), l3 = lvl_[3].next() * nc,
                    l4 = lvl_[4].next();
        const float r = res_.next(), gain = inGain_.next(), postIn = postIn_.next(), postOut = postOut_.next(),
                    vca = vca_.next();
        const bool fade = xfLeft_ > 0;   // a slope change, crossfading the taps
        const float xf = fade ? static_cast<float>(xfLeft_--) * (1.0f / kTapFade) : 0.0f;   // of the old tap
        float hi[kOversample];
        for (int k = 0; k < kOversample; ++k) {
            // Cutoff and VCA move per base sample; the first half-step goes halfway.
            const float fk = k == 0 ? 0.5f * (fq[i] + fq[i + 1]) : fq[i + 1];
            const float amp = (k == 0 ? 0.5f * (ae[i] + ae[i + 1]) : ae[i + 1]) * vca;
            float v1, v2, vs;
            if (resetPending_) {   // keyboard reset: every oscillator restarts its cycle here
                v1 = osc_[0].tickReset(dt1, s1, 1.0f);
                v2 = osc_[1].tickReset(dt2, s2, 1.0f);
                sub_.reset(subOct);
                vs = sub_.tick(false, 0.0f, subOct);
                resetPending_ = false;
            } else {
                bool w1 = false, w2 = false;
                float x1 = 0.0f, x2 = 0.0f;
                v1 = osc_[0].tick(dt1, s1, w1, x1);
                v2 = sync && w1 ? osc_[1].tickReset(dt2, s2, x1) : osc_[1].tick(dt2, s2, w2, x2);
                vs = sub_.tick(w1, x1, subOct);
            }
            const float white = randBipolar(noiseRng_);
            float mix = l0 * v1 + l1 * vs + l2 * v2;
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
