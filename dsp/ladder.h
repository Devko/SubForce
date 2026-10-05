#pragma once
// The transistor ladder: four one-pole stages with a tanh at the input pair and at each stage,
// resonance fed back from the fourth. Zero-delay feedback with the nonlinear gains taken from
// the previous sample (Teemu "mystran" Voipio's "cheap non-linear zero-delay filters", KVR 2012):
// every sample solves the linearised loop exactly, so it stays stable at any cutoff and
// resonance, and it keeps the ladder's habits:
//   - the passband drops as resonance rises (the "bass loss": DC gain 1 / (1 + r))
//   - hot input saturates inside the loop, not just before it
//   - past r = 4 it self-oscillates, the tanh stages holding the amplitude
// Run at 2x (dsp/synth.cpp); taps after stage 1..4 give the 6/12/18/24 dB slopes, the feedback
// always from stage 4 as in the circuit.
#include "fastmath.h"

namespace sf {

struct Ladder {
    float s[4] = {};    // the stages' integrator states
    float zi = 0.0f;    // the previous input (the nonlinearity sees the half-sample midpoint)

    // in: input; f = tan(pi fc / fs); r: feedback, 0 .. ~4.6. y[0..3]: the four stage outputs.
    void tick(float in, float f, float r, float* y) {
        const float ih = 0.5f * (in + zi);
        zi = in;
        // Nonlinear gains, from the previous sample's states.
        const float t0 = tanhXdX(ih - r * s[3]);
        const float t1 = tanhXdX(s[0]);
        const float t2 = tanhXdX(s[1]);
        const float t3 = tanhXdX(s[2]);
        const float t4 = tanhXdX(s[3]);
        // Each stage's denominator, and the feedback path factored out.
        const float g0 = 1.0f / (1.0f + f * t1), g1 = 1.0f / (1.0f + f * t2);
        const float g2 = 1.0f / (1.0f + f * t3), g3 = 1.0f / (1.0f + f * t4);
        const float f3 = f * t3 * g3, f2 = f * t2 * g2 * f3, f1 = f * t1 * g1 * f2, f0 = f * t0 * g0 * f1;
        // The loop, solved for the last stage, then the others from it.
        const float y3 = (g3 * s[3] + f3 * g2 * s[2] + f2 * g1 * s[1] + f1 * g0 * s[0] + f0 * in) / (1.0f + r * f0);
        const float xx = t0 * (in - r * y3);
        const float y0 = t1 * g0 * (s[0] + f * xx);
        const float y1 = t2 * g1 * (s[1] + f * y0);
        const float y2 = t3 * g2 * (s[2] + f * y1);
        s[0] += 2.0f * f * (xx - y0);
        s[1] += 2.0f * f * (y0 - y1);
        s[2] += 2.0f * f * (y1 - y2);
        s[3] += 2.0f * f * (y2 - t4 * y3);
        y[0] = y0;
        y[1] = y1;
        y[2] = y2;
        y[3] = y3;
    }

    void reset() {
        s[0] = s[1] = s[2] = s[3] = 0.0f;
        zi = 0.0f;
    }
};

} // namespace sf
