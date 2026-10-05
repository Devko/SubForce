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
constexpr float kFeedbackGain = 1.6f;    // the feedback level knob at full
constexpr float kOutGain = 1.2f;         // engine output at 0 dB volume
constexpr float kMaxCutoff = 0.40f;      // of the 2x rate (35 kHz): the ladder's coefficient stays sane
constexpr float kMinCutoff = 8.0f;       // Hz
constexpr float kThermal = 1e-4f;        // the ladder's input noise floor (-80 dB)

// Mixer knobs: an audio taper.
inline float taper(float k) {
    k = clampf(k, 0.0f, 1.0f);
    return k * k;
}

inline float noteOf(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

} // namespace

Synth::Synth(float sampleRate)
    : sr_(sampleRate), osr_(sampleRate * kOversample), invOsr_(1.0f / (sampleRate * kOversample)) {
    setPatch(Patch{});
}

void Synth::setPatch(const Patch& p) {
    patch_ = p;
    havePatch_ = true;
    fc_ = envCoef(p.fenv, sr_, kbScale(p.fenv.kb));
    ac_ = envCoef(p.aenv, sr_, kbScale(p.aenv.kb));
    noiseK_ = exp2Fast(-6.0f * clampf(p.noiseColor, 0.0f, 1.0f));   // white .. a ~220 Hz one-pole
    noiseComp_ = std::sqrt((2.0f - noiseK_) / noiseK_);              // the same loudness at every colour
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
    holdKey(std::clamp(note, 0, 127), velocity);
    update();
}

void Synth::noteOff(int note) {
    const int before = nHeld_;
    dropKey(note);
    if (nHeld_ != before) update();
}

// The keys changed: move the oscillators, open or close the gate, retrigger as the trigger
// mode says. Legato (another key was down): Single keeps the envelopes going; Legato glide
// glides only then.
void Synth::update() {
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
    if (!fresh && n1 == note1_ && n2 == note2_) return;   // the sounding keys didn't change
    // Always glides from the last note played; the very first note has none to come from.
    const bool glide = havePitch_ && (patch_.glideMode == GL_ALWAYS || (patch_.glideMode == GL_LEGATO && !fresh));
    havePitch_ = true;
    note1_ = n1;
    note2_ = n2;
    glideTo(glide_[0], static_cast<float>(n1), glide && patch_.glideDest != OD_OSC2);
    glideTo(glide_[1], static_cast<float>(n2), glide && patch_.glideDest != OD_OSC1);
    gate_ = true;
    if (fresh || patch_.trigger == TR_MULTI) trigger(vel);
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
    if (silent_) {
        silent_ = false;
        aePrev_ = 0.0f;
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
    if (patch_.glideType == GT_EXP) {   // RC: 99% of the way at glideTime
        g.exp = true;
        g.k = 1.0f - std::exp(-4.6f / (patch_.glideTime * sr_));
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
        g.pitch += (g.target - g.pitch) * (1.0f - std::pow(1.0f - g.k, static_cast<float>(n)));
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

void Synth::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
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
    const double div = kSyncBeats[std::clamp(m.div, 0, kNumSyncDivs - 1)];
    auto newCycle = [&] {
        b.held = randBipolar(rng_);
        b.from = b.to;
        b.to = randBipolar(rng_);
    };
    if (m.sync && !m.retrig && playing_ && beatsValid_) {   // locked to MPC's bar position
        double ph = beats_ / div;
        ph -= std::floor(ph);
        if (static_cast<float>(ph) < b.phase) newCycle();
        b.phase = static_cast<float>(ph);
    } else {
        const float hz = (m.sync ? static_cast<float>(bpm_ / 60.0 / div) : m.rateHz) * b.rateMul;
        float next = b.phase + hz * static_cast<float>(n) / sr_;
        if (next >= 1.0f) {
            next -= std::floor(next);
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
    d.v += (d.target - d.v) * std::min(1.0f, static_cast<float>(n) / (0.6f * sr_));
    return d.v;
}

// Every kControl samples: glide, the mod busses, drift, and the targets every control value
// glides to over the next kControl samples.
void Synth::control() {
    const int n = sinceCtl_ > 0 ? sinceCtl_ : kControl;
    sinceCtl_ = 0;
    const Patch& p = patch_;
    beats_ += static_cast<double>(n) * bpm_ / 60.0 / static_cast<double>(sr_);
    stepGlide(glide_[0], n);
    stepGlide(glide_[1], n);
    fVel_ = 1.0f - p.fenv.vel + p.fenv.vel * vel_;

    float pitchMod[2] = {}, cutMod = 0.0f, waveMod[2] = {}, resMod = 0.0f, driveMod = 0.0f, lvlMod[5] = {};
    float volMul = 1.0f, otherRate[2] = {1.0f, 1.0f};
    for (int b = 0; b < 2; ++b) {
        const ModPatch& m = p.mod[b];
        const float depth = m.control == MC_MODWHEEL ? wheel_ : m.control == MC_AFTERTOUCH ? pressure_
                          : m.control == MC_VELOCITY ? vel_ : 1.0f;
        const float out = busValue(bus_[b], m, n) * depth;
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

    const float driftCents[2] = {driftStep(drift_[0], n) * p.drift * 6.0f + noteDrift_[0],
                                 driftStep(drift_[1], n) * p.drift * 6.0f + noteDrift_[1]};
    const float driftCut = driftStep(drift_[2], n) * p.drift * 0.6f;
    const float bend = bend_ * (bend_ > 0.0f ? p.bendUp : p.bendDown);
    const float pitch[2] = {
        glide_[0].pitch + 12.0f * static_cast<float>(p.osc[0].octave) + bend + pitchMod[0] + 0.01f * driftCents[0],
        glide_[1].pitch + 12.0f * static_cast<float>(p.osc[1].octave) + p.osc2Semis + bend + pitchMod[1] + 0.01f * driftCents[1]};
    const float drive = clampf(p.drive + driveMod, 0.0f, 1.0f);
    const float driveGain = 1.0f + kDriveSpan * drive * drive;
    const float aVel = 1.0f - p.aenv.vel + p.aenv.vel * vel_;

    const float target[] = {
        clampf(noteHz(pitch[0]) * invOsr_, 1e-7f, 0.45f),
        clampf(noteHz(pitch[1]) * invOsr_, 1e-7f, 0.45f),
        clampf(p.osc[0].wave + waveMod[0], 0.0f, 1.0f),
        clampf(p.osc[1].wave + waveMod[1], 0.0f, 1.0f),
        noteOf(p.cutoffHz) + p.keyTrack * (glide_[0].pitch - 60.0f) + cutMod + driftCut,
        envSemis(p.envAmount) * fVel_,
        kResMax * clampf(p.res + resMod, 0.0f, 1.0f),
        kInGain * driveGain,
        1.0f + 2.0f * drive,
        taper(p.mixOsc1 + lvlMod[0]),
        taper(p.mixSub + lvlMod[1]),
        taper(p.mixOsc2 + lvlMod[2]),
        taper(p.mixNoise + lvlMod[3]),
        taper(p.mixFeedback + lvlMod[4]) * kFeedbackGain,
        kOutGain * aVel * volMul / std::pow(driveGain, 0.35f),
        exp2Fast(clampf(p.volumeDb, -60.0f, 12.0f) * (1.0f / 6.0206f)) * (p.volumeDb <= -59.5f ? 0.0f : 1.0f),
    };
    Ramp* ramps[] = {&dt_[0], &dt_[1], &wave_[0], &wave_[1], &cut_, &egAmt_, &res_, &inGain_, &post_,
                     &lvl_[0], &lvl_[1], &lvl_[2], &lvl_[3], &lvl_[4], &vca_, &vol_};
    static_assert(sizeof target / sizeof target[0] == sizeof ramps / sizeof ramps[0], "a target per ramp");
    constexpr float invN = 1.0f / static_cast<float>(kControl);
    for (size_t i = 0; i < sizeof ramps / sizeof ramps[0]; ++i) {
        // A pitch jump (a new note without glide) lands at once; everything else glides. The
        // very first step starts every value where it belongs.
        if (snapAll_ || (snap_ && i < 2)) ramps[i]->snap(target[i]);
        else ramps[i]->to(target[i], invN);
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
    fbIn_ = fbX1_ = fbY1_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    noiseLp_ = 0.0f;
    aePrev_ = 0.0f;
}

// --- audio --------------------------------------------------------------------------------

void Synth::render(float* outL, float* outR, int n) {
    StageClock clock;
    int pos = 0;
    while (pos < n) {
        if (ctlLeft_ <= 0) {
            control();
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
// release, the control values arrive where they were going.
void Synth::renderSilent(float* out, int n) {
    for (int i = 0; i < n; ++i) out[i] = 0.0f;
    for (int o = 0; o < 2; ++o) {
        const float d = dt_[o].v * static_cast<float>(kOversample * n);
        osc_[o].t += d - std::floor(osc_[o].t + d);
    }
    for (Ramp* r : {&dt_[0], &dt_[1], &wave_[0], &wave_[1], &cut_, &egAmt_, &res_, &inGain_, &post_,
                    &lvl_[0], &lvl_[1], &lvl_[2], &lvl_[3], &lvl_[4], &vca_, &vol_})
        r->skip(n);
    for (int i = 0; i < n; ++i) fenv_.tick(fc_);
}

void Synth::renderRun(float* out, int n) {
    const int tap = std::clamp(patch_.slope, 0, 3);
    const int subOct = patch_.subOctave == SO_TWO ? 2 : 1;
    const bool sync = patch_.sync;
    const float nk = noiseK_, nc = noiseComp_;
    const float fbR = 1.0f - 2.0f * kPi * 15.0f * invOsr_;     // feedback DC blocker, 15 Hz
    const float dcR = 1.0f - 2.0f * kPi * 5.0f / sr_;          // output DC blocker, 5 Hz
    const float maxHz = kMaxCutoff * osr_;
    float peak = peak_;
    for (int i = 0; i < n; ++i) {
        const float dt1 = dt_[0].next(), dt2 = dt_[1].next();
        const Shape s1 = shapeOf(wave_[0].next()), s2 = shapeOf(wave_[1].next());
        const float l0 = lvl_[0].next(), l1 = lvl_[1].next(), l2 = lvl_[2].next(), l3 = lvl_[3].next() * nc,
                    l4 = lvl_[4].next();
        const float fe = fenv_.tick(fc_);
        const float ae = aenv_.tick(ac_);
        const float cs = cut_.next() + egAmt_.next() * fe;
        const float f = tanFast(kPi * clampf(noteHz(cs), kMinCutoff, maxHz) * invOsr_);
        const float r = res_.next(), gain = inGain_.next(), post = post_.next(), vca = vca_.next();
        const float postIn = 0.5f * post, postOut = 2.0f / post;
        float hi[kOversample];
        for (int k = 0; k < kOversample; ++k) {
            // Cutoff and VCA move per base sample; the first half-step goes halfway.
            const float fk = k == 0 ? 0.5f * (fPrev_ + f) : f;
            const float amp = (k == 0 ? 0.5f * (aePrev_ + ae) : ae) * vca;
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
            noiseLp_ += (white - noiseLp_) * nk;
            const float mix = l0 * v1 + l1 * vs + l2 * v2 + l3 * noiseLp_ + l4 * fbIn_;
            float y[4];
            // The circuit's own noise floor (-80 dB): what starts a self-oscillating filter with
            // every source down, as on the hardware.
            ladder_.tick(mix * gain + kThermal * white, fk, r, y);
            const float v = softclip(y[tap] * postIn) * postOut * amp;   // Multidrive's second stage, VCA
            fbY1_ = v - fbX1_ + fbR * fbY1_;   // the feedback path: DC blocked, back into the mixer next sample
            fbX1_ = v;
            fbIn_ = fbY1_;
            hi[k] = v;
        }
        fPrev_ = f;
        aePrev_ = ae;
        const float lo = dec_.process(hi[0], hi[1]);
        dcY1_ = lo - dcX1_ + dcR * dcY1_;
        dcX1_ = lo;
        const float o = dcY1_ * vol_.next();
        peak = std::max(peak, std::fabs(o));
        out[i] = o;
    }
    peak_ = peak;
}

} // namespace sf
