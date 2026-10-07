#pragma once
// The DAHDSR envelope: Delay, Attack, Hold, Decay, Sustain, Release, as the original's: a linear
// attack (its default) or an exponential one (its EXP ATTACK: an RC charging toward 1.5 and
// stopping at 1, steep at first), decay settling exponentially on the sustain level, release
// falling to -80 dB. Loop, while the key is held: delay -> attack -> hold
// -> decay -> release, and round again, the release stage included as on the original ("delay,
// attack, hold, decay, and release stages will loop continuously"): with sustain at 0 it is
// D-A-H-D; with sustain up, decay falls to it and release takes it the rest of the way.
#include "fastmath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sf {

struct EnvTimes {
    float delay = 0.0f, attack = 0.002f, hold = 0.0f, decay = 0.4f, sustain = 0.5f, release = 0.2f;   // seconds; sustain 0..1
    bool  loop = false;
    bool  expAttack = false;
};

constexpr float kExpAttackTarget = 1.5f;   // where the exponential attack heads (it stops at 1)

struct EnvCoef {
    int   delayN = 0, holdN = 0;            // samples
    float att = 1.0f;                       // per-sample attack step: linear, 1 in `attack`; exponential,
                                            // the one-pole step that reaches 1 (from 0) in `attack`
    float dec = 1.0f, rel = 1.0f, sus = 0.5f;   // per-sample one-pole steps
    bool  loop = false;
    bool  expAttack = false;
};

// Per-sample one-pole step for a time constant of tau seconds.
inline float onePole(float tau, float sr) { return 1.0f - std::exp(-1.0f / ((tau > 1e-5f ? tau : 1e-5f) * sr)); }

// scale multiplies every time (keyboard tracking).
inline EnvCoef envCoef(const EnvTimes& e, float sr, float scale) {
    EnvCoef c;
    c.delayN = static_cast<int>(std::lround(clampf(e.delay * scale, 0.0f, 60.0f) * sr));
    c.holdN = static_cast<int>(std::lround(clampf(e.hold * scale, 0.0f, 60.0f) * sr));
    c.expAttack = e.expAttack;
    // Linear: 0 to 1 in `attack`. Exponential: 1.5 (1 - e^(-t / tau)) reaches 1 at t = tau ln 3.
    c.att = e.expAttack ? onePole(e.attack * scale / 1.098612289f, sr) : 1.0f / (std::max(e.attack * scale, 1e-5f) * sr);
    c.dec = onePole(e.decay * scale / 6.9078f, sr);    // ln(1000): 60 dB of the way at `decay`
    c.rel = onePole(e.release * scale / 6.9078f, sr);
    c.sus = clampf(e.sustain, 0.0f, 1.0f);
    c.loop = e.loop;
    return c;
}

enum EnvStage : uint8_t { E_IDLE, E_DELAY, E_ATTACK, E_HOLD, E_DECAY, E_RELEASE };

struct Env {
    EnvStage stage = E_IDLE;
    float    v = 0.0f;
    int      count = 0;          // samples left in Delay / Hold
    bool     looping = false;    // in a loop's release stage (the key still held)

    // A new note. reset: the attack starts from 0, else from where the envelope is (analog).
    void trigger(const EnvCoef& c, bool reset) {
        if (reset) v = 0.0f;
        stage = c.delayN > 0 ? E_DELAY : E_ATTACK;
        count = c.delayN;
        looping = false;
    }
    void release() {
        if (stage != E_IDLE) stage = E_RELEASE;
        looping = false;
    }

    float tick(const EnvCoef& c) {
        switch (stage) {
            case E_IDLE: break;
            case E_DELAY:
                if (--count <= 0) stage = E_ATTACK;
                break;
            case E_ATTACK:
                v += c.expAttack ? (kExpAttackTarget - v) * c.att : c.att;
                if (v >= 1.0f) {
                    v = 1.0f;
                    stage = c.holdN > 0 ? E_HOLD : E_DECAY;
                    count = c.holdN;
                }
                break;
            case E_HOLD:
                if (--count <= 0) stage = E_DECAY;
                break;
            case E_DECAY:
                v += (c.sus - v) * c.dec;
                if (c.loop && v - c.sus < 0.01f) {   // on the sustain level: the loop's release
                    stage = E_RELEASE;
                    looping = true;
                }
                break;
            case E_RELEASE:
                v -= v * c.rel;
                if (looping && v < 0.01f) {   // -40 dB: round again (the key is still held)
                    stage = c.delayN > 0 ? E_DELAY : E_ATTACK;
                    count = c.delayN;
                    looping = false;
                } else if (v < 1e-4f) {
                    v = 0.0f;
                    stage = E_IDLE;
                }
                break;
        }
        return v;
    }
};

} // namespace sf
