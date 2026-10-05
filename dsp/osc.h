#pragma once
// The oscillators: a continuously variable wave (triangle -> saw -> square -> narrow pulse),
// hard sync, and the square sub oscillator, band-limited with 2-point polyBLEP (steps) and
// polyBLAMP (corners) at the 2x rate.
//
// Each output runs one sample late: a discontinuity between samples n-1 and n corrects both,
// so sample n-1 is only finished (and returned) once sample n's phase has been worked out. The
// one high-rate sample of latency (11 us) is what makes sync and the keyboard reset exact.
#include "fastmath.h"

namespace sf {

// The wave knob as weights of three naive shapes, all from -1 to +1 over a cycle of phase t:
//   triangle  rises -1 -> 1 over t in [0, 0.5), falls back over [0.5, 1)
//   saw       2t - 1 (rises, drops by 2 at the wrap)
//   pulse     +1 for t < pw, else -1, minus its mean (AC coupled, like the analog output)
// 0 = triangle, 1/3 = saw, 2/3 = square, 1 = a 6% pulse; crossfades in between.
struct Shape {
    float tri = 0.0f, saw = 1.0f, pul = 0.0f;
    float pw = 0.5f;    // pulse width
    float dc = 0.0f;    // pul * (2 pw - 1): the pulse's mean, removed
};

constexpr float kMinPulse = 0.06f;

inline Shape shapeOf(float m) {
    Shape s;
    m = clampf(m, 0.0f, 1.0f) * 3.0f;
    if (m < 1.0f) {
        s.tri = 1.0f - m;
        s.saw = m;
        s.pul = 0.0f;
    } else if (m < 2.0f) {
        s.tri = 0.0f;
        s.saw = 2.0f - m;
        s.pul = m - 1.0f;
    } else {
        s.tri = s.saw = 0.0f;
        s.pul = 1.0f;
        s.pw = 0.5f - (m - 2.0f) * (0.5f - kMinPulse);
    }
    s.dc = s.pul * (2.0f * s.pw - 1.0f);
    return s;
}

inline float waveValue(const Shape& s, float t) {
    const float tri = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
    const float pul = t < s.pw ? 1.0f : -1.0f;
    return s.tri * tri + s.saw * (2.0f * t - 1.0f) + s.pul * pul - s.dc;
}

// d value / d phase (the pulse is flat between its edges).
inline float waveSlope(const Shape& s, float t) { return s.tri * (t < 0.5f ? 4.0f : -4.0f) + 2.0f * s.saw; }

// The two samples a discontinuity touches: `pending` is sample n-1 (not yet returned), `cur`
// collects corrections for sample n. x = how far before sample n the event happened, in
// samples, 0 <= x <= 1.
struct Blep {
    float pending = 0.0f, cur = 0.0f;
    // The value jumps by h.
    void step(float h, float x) {
        const float y = 1.0f - x;
        pending += 0.5f * h * x * x;
        cur -= 0.5f * h * y * y;
    }
    // The slope changes by s (value per sample).
    void corner(float s, float x) {
        const float y = 1.0f - x;
        pending += s * (1.0f / 6.0f) * x * x * x;
        cur += s * (1.0f / 6.0f) * y * y * y;
    }
};

// Moves phase t forward by d (d <= dt, the phase per sample) on a stretch that ends `end`
// samples before sample n, correcting every discontinuity it crosses. Returns true if it
// wrapped; *wrapX is then the wrap's distance to sample n.
inline bool advance(float& t, float d, float dt, float end, const Shape& s, Blep& b, float* wrapX) {
    const float inv = 1.0f / dt;
    auto at = [&](float t1, float p) { return clampf(end + (t1 - p) * inv, 0.0f, 1.0f); };
    float t1 = t + d;
    bool wrapped = false;
    if (t1 >= 1.0f) {
        // Edges before the wrap, then the wrap itself.
        if (s.pul != 0.0f && t < s.pw) b.step(-2.0f * s.pul, at(t1, s.pw));
        if (s.tri != 0.0f && t < 0.5f) b.corner(-8.0f * s.tri * dt, at(t1, 0.5f));
        const float x = at(t1, 1.0f);
        b.step(2.0f * (s.pul - s.saw), x);
        if (s.tri != 0.0f) b.corner(8.0f * s.tri * dt, x);
        if (wrapX) *wrapX = x;
        wrapped = true;
        t1 -= 1.0f;
        t = 0.0f;   // edges after the wrap (a very narrow pulse at a high pitch) are checked from 0
    }
    if (s.pul != 0.0f && t < s.pw && t1 >= s.pw) b.step(-2.0f * s.pul, at(t1, s.pw));
    if (s.tri != 0.0f && t < 0.5f && t1 >= 0.5f) b.corner(-8.0f * s.tri * dt, at(t1, 0.5f));
    t = t1;
    return wrapped;
}

// One oscillator. tick() returns sample n-1 and works out sample n.
struct Osc {
    float t = 0.0f;   // phase 0..1
    Blep  b;

    // Free running. wrapX: where it wrapped (for sync and the sub), if it did.
    float tick(float dt, const Shape& s, bool& wrapped, float& wrapX) {
        b.cur = 0.0f;
        wrapped = advance(t, dt, dt, 0.0f, s, b, &wrapX);
        const float out = b.pending;
        b.pending = waveValue(s, t) + b.cur;
        return out;
    }

    // Reset to phase 0 at distance x before sample n (hard sync: the master's wrap; the keyboard
    // reset: x = 1, exactly at sample n-1).
    float tickReset(float dt, const Shape& s, float x) {
        b.cur = 0.0f;
        advance(t, (1.0f - x) * dt, dt, x, s, b, nullptr);   // up to the reset
        b.step(waveValue(s, 0.0f) - waveValue(s, t), x);
        b.corner((waveSlope(s, 0.0f) - waveSlope(s, t)) * dt, x);
        t = 0.0f;
        advance(t, x * dt, dt, 0.0f, s, b, nullptr);         // and on from 0
        const float out = b.pending;
        b.pending = waveValue(s, t) + b.cur;
        return out;
    }
};

// The square sub oscillator: one or two octaves under oscillator 1, flipping on its wraps.
struct Sub {
    uint32_t count = 0;   // oscillator 1's cycles
    Blep     b;

    static float level(uint32_t c, int octaves) { return (c >> (octaves - 1)) & 1u ? -1.0f : 1.0f; }

    float tick(bool wrapped, float x, int octaves) {
        b.cur = 0.0f;
        if (wrapped) {
            const float was = level(count, octaves);
            ++count;
            const float now = level(count, octaves);
            if (now != was) b.step(now - was, x);
        }
        const float out = b.pending;
        b.pending = level(count, octaves) + b.cur;
        return out;
    }

    // With the keyboard reset: back to the start of its cycle, at sample n-1 (x = 1).
    void reset(int octaves) {
        const float was = level(count, octaves);
        count = 0;
        if (was != 1.0f) b.step(1.0f - was, 1.0f);
    }
};

} // namespace sf
