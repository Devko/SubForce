#pragma once
// The DAHDSR envelope: Delay, Attack, Hold, Decay, Sustain, Release, as analog RC segments.
// Attack charges toward 1.2 and stops at 1.0 (the convex rise of a real attack); decay settles
// exponentially on the sustain level; release falls to -80 dB. Loop: while the key is held the
// envelope cycles delay -> attack -> hold -> decay, turning back once decay is near sustain.
#include "fastmath.h"

#include <cmath>
#include <cstdint>

namespace sf {

struct EnvTimes {
    float delay = 0.0f, attack = 0.002f, hold = 0.0f, decay = 0.4f, sustain = 0.5f, release = 0.2f;   // seconds; sustain 0..1
    bool  loop = false;
};

struct EnvCoef {
    int   delayN = 0, holdN = 0;            // samples
    float att = 1.0f, dec = 1.0f, rel = 1.0f, sus = 0.5f;   // per-sample one-pole steps
    bool  loop = false;
};

// Per-sample one-pole step for a time constant of tau seconds.
inline float onePole(float tau, float sr) { return 1.0f - std::exp(-1.0f / ((tau > 1e-5f ? tau : 1e-5f) * sr)); }

// scale multiplies every time (keyboard tracking).
inline EnvCoef envCoef(const EnvTimes& e, float sr, float scale) {
    EnvCoef c;
    c.delayN = static_cast<int>(std::lround(clampf(e.delay * scale, 0.0f, 60.0f) * sr));
    c.holdN = static_cast<int>(std::lround(clampf(e.hold * scale, 0.0f, 60.0f) * sr));
    c.att = onePole(e.attack * scale / 1.7918f, sr);   // ln(1.2 / 0.2): the curve crosses 1.0 at `attack`
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
    int      count = 0;   // samples left in Delay / Hold

    // A new note. reset: the attack starts from 0, else from where the envelope is (analog).
    void trigger(const EnvCoef& c, bool reset) {
        if (reset) v = 0.0f;
        stage = c.delayN > 0 ? E_DELAY : E_ATTACK;
        count = c.delayN;
    }
    void release() {
        if (stage != E_IDLE) stage = E_RELEASE;
    }

    float tick(const EnvCoef& c) {
        switch (stage) {
            case E_IDLE: break;
            case E_DELAY:
                if (--count <= 0) stage = E_ATTACK;
                break;
            case E_ATTACK:
                v += (1.2f - v) * c.att;
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
                if (c.loop && v - c.sus < 0.01f) {   // near the floor: round again
                    stage = c.delayN > 0 ? E_DELAY : E_ATTACK;
                    count = c.delayN;
                }
                break;
            case E_RELEASE:
                v -= v * c.rel;
                if (v < 1e-4f) {
                    v = 0.0f;
                    stage = E_IDLE;
                }
                break;
        }
        return v;
    }
};

} // namespace sf
