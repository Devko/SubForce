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
//
// The work is split by its shape (dsp/simd.h; measured on the Force, docs/PERFORMANCE.md):
//   - the four stages' nonlinear gains and their 1 / (1 + f t) are independent: four vector
//     lanes, NEON reciprocals (ARMv7 has no vector divide, and VFP's one divider is unpipelined:
//     the scalar version queued 10 divisions per sample behind each other);
//   - each stage's output is affine in the previous one's, y_k = a_k + b_k y_k-1, so the loop is
//     solved as y_k = al_k + be_k y3 by a short scalar chain, closed by the last stage with one
//     division, and every stage's input and output then come out in one vector step.
#include "fastmath.h"
#include "simd.h"

namespace sf {

struct Ladder {
    float s[4] = {};    // the stages' integrator states
    float zi = 0.0f;    // the previous input (the nonlinearity sees the half-sample midpoint)

    // in: input; f = tan(pi fc / fs); r: feedback, 0 .. ~4.6. y[0..3]: the four stage outputs.
    void tick(float in, float f, float r, float* y) {
        const float ih = 0.5f * (in + zi);
        zi = in;
        const f4 S = load4(s), F = splat(f), top = f4{0.0f, 0.0f, 0.0f, 1.0f}, low = splat(1.0f) - top;
        // Nonlinear gains, from the previous sample's states: t1..t4 of the stages, t0 of the
        // input pair (lane 0 of its own vector: as cheap as one lane, and no divider).
        const f4 T = tanhXdX4(S);
        const float t0 = tanhXdX4(splat(ih - r * s[3]))[0];
        // Stage k solved for its own state: u_k = g_k (s_k + f y_k-1), g_k = 1 / (1 + f t_k+1); it
        // passes on y_k = t_k+1 u_k (tanh'd), the last stage its raw u_3: y_k = a_k + b_k y_k-1.
        const f4 MG = (T * low + top) * recip4<2>(splat(1.0f) + F * T);   // t1 g0, t2 g1, t3 g2, g3
        const f4 A = MG * S, B = MG * F;
        // The input pair's output x = t0 (in - r y3), and every stage after it, as al + be y3...
        const float alx = t0 * in, bex = -t0 * r;
        const float al0 = A[0] + B[0] * alx, be0 = B[0] * bex;
        const float al1 = A[1] + B[1] * al0, be1 = B[1] * be0;
        const float al2 = A[2] + B[2] * al1, be2 = B[2] * be1;
        // ...closed by the last stage, y3 = a3 + b3 y2 (1 - b3 be2 = 1 + r f^4 t0..t3 g0..g3 > 0).
        const float y3 = (A[3] + B[3] * al2) / (1.0f - B[3] * be2);
        const f4 X = f4{alx, al0, al1, al2} + f4{bex, be0, be1, be2} * splat(y3);   // each stage's input
        const f4 Y = ext<1>(X, splat(y3));                                             // ...and output
        store4(y, Y);
        // Trapezoidal integrators: s += 2 f (input - output), the last stage's output tanh'd.
        store4(s, S + (F + F) * (X - Y * (T * top + low)));
    }

    void reset() {
        s[0] = s[1] = s[2] = s[3] = 0.0f;
        zi = 0.0f;
    }
};

} // namespace sf
