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
// samples before sample n, while the pulse width moves from pw0 to pw1 across it, correcting
// every discontinuity on the way: the wrap, the triangle's corner at 0.5, and the pulse edge
// wherever phase and width cross -- the phase passing the width (falling), or the width
// sweeping past the phase (a modulated width: either way). Returns true if it wrapped; *wrapX
// is then the wrap's distance to sample n.
inline bool advance(float& t, float d, float dt, float pw0, float pw1, float end, const Shape& s, Blep& b,
                    float* wrapX) {
    const float len = d / dt;   // the stretch, in samples
    // Something at position u (0..1) along the stretch: its distance to sample n.
    auto xAt = [&](float u) { return clampf(end + (1.0f - u) * len, 0.0f, 1.0f); };
    const float t0 = t, t1 = t + d;
    const bool wraps = t1 >= 1.0f;
    const float uw = wraps ? (1.0f - t0) / d : 1.0f;   // where it wraps (d > 0 if it does)
    if (s.pul != 0.0f) {
        // g = phase - width, linear on each side of the wrap: the edge is where it changes sign.
        auto edge = [&](float g0, float g1, float u0, float u1) {
            if ((g0 < 0.0f) == (g1 < 0.0f)) return;
            const float u = u0 + (u1 - u0) * g0 / (g0 - g1);
            b.step(g0 < 0.0f ? -2.0f * s.pul : 2.0f * s.pul, xAt(u));
        };
        const float pwW = pw0 + (pw1 - pw0) * uw;
        edge(t0 - pw0, (wraps ? 1.0f : t1) - pwW, 0.0f, uw);
        if (wraps) edge(-pwW, t1 - 1.0f - pw1, uw, 1.0f);
    }
    if (s.tri != 0.0f) {
        if (t0 < 0.5f && t1 >= 0.5f) b.corner(-8.0f * s.tri * dt, xAt((0.5f - t0) / d));
        if (t1 >= 1.5f) b.corner(-8.0f * s.tri * dt, xAt((1.5f - t0) / d));
    }
    if (wraps) {
        const float x = xAt(uw);
        b.step(2.0f * (s.pul - s.saw), x);   // saw falls by 2, the pulse rises back above its width
        if (s.tri != 0.0f) b.corner(8.0f * s.tri * dt, x);
        if (wrapX) *wrapX = x;
    }
    t = wraps ? t1 - 1.0f : t1;
    return wraps;
}

// One oscillator. tick() returns sample n-1 and works out sample n.
struct Osc {
    float t = 0.0f;    // phase 0..1
    float pw = 0.5f;   // the pulse width sample n-1 was worked out with
    Blep  b;

    // Free running. wrapX: where it wrapped (for sync and the sub), if it did.
    float tick(float dt, const Shape& s, bool& wrapped, float& wrapX) {
        b.cur = 0.0f;
        wrapped = advance(t, dt, dt, pw, s.pw, 0.0f, s, b, &wrapX);
        pw = s.pw;
        const float out = b.pending;
        b.pending = waveValue(s, t) + b.cur;
        return out;
    }

    // Reset to phase 0 at distance x before sample n (hard sync: the master's wrap; the keyboard
    // reset: x = 1, exactly at sample n-1).
    float tickReset(float dt, const Shape& s, float x) {
        b.cur = 0.0f;
        Shape at = s;   // the shape at the reset, its width where the sweep has got to
        at.pw = pw + (s.pw - pw) * (1.0f - x);
        at.dc = at.pul * (2.0f * at.pw - 1.0f);
        advance(t, (1.0f - x) * dt, dt, pw, at.pw, x, s, b, nullptr);   // up to the reset
        b.step(waveValue(at, 0.0f) - waveValue(at, t), x);
        b.corner((waveSlope(at, 0.0f) - waveSlope(at, t)) * dt, x);
        t = 0.0f;
        advance(t, x * dt, dt, at.pw, s.pw, 0.0f, s, b, nullptr);   // and on from 0
        pw = s.pw;
        const float out = b.pending;
        b.pending = waveValue(s, t) + b.cur;
        return out;
    }
};

// The square sub oscillator: one or two octaves under oscillator 1, flipping on its wraps.
struct Sub {
    uint32_t count = 0;   // oscillator 1's cycles
    int      oct = 1;     // the octaves sample n-1 was worked out with
    Blep     b;

    static float level(uint32_t c, int octaves) { return (c >> (octaves - 1)) & 1u ? -1.0f : 1.0f; }

    float tick(bool wrapped, float x, int octaves) {
        b.cur = 0.0f;
        if (octaves != oct) {   // switched mid-note: a band-limited step, not a naive one
            const float was = level(count, oct), now = level(count, octaves);
            if (now != was) b.step(now - was, 1.0f);
            oct = octaves;
        }
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
        const float was = level(count, oct);
        count = 0;
        oct = octaves;
        if (was != 1.0f) b.step(1.0f - was, 1.0f);
    }
};

} // namespace sf
