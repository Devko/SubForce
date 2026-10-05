#pragma once
// The 2x decimator: the engine runs its oscillators, ladder and drive at 88.2 kHz and this folds
// the result back to MPC's 44.1 kHz. A polyphase IIR halfband (two chains of first-order
// allpasses at the low rate, Laurent de Soras's HIIR structure): 8 multiplies per output sample.
//
// Design: 8 coefficients, transition band 0.0232 of the high rate (passband to 20.0 kHz,
// stopband from 24.1 kHz): passband ripple < 1e-7 dB, stopband >= 85 dB. Coefficients from
// HIIR's PolyphaseIir2Designer formulas (tools/halfband_design.py prints them); the response
// is measured in test/engine_test.cpp. Nonlinear phase (an IIR), like any analog filter.
namespace sf {

class Decimator {
public:
    static constexpr int kCoefs = 8;

    // Two high-rate samples, `early` first, become one low-rate sample.
    float process(float early, float late) {
        float a = late, b = early;   // a: the even chain (coefs 0, 2, ...), b: the odd chain (1, 3, ...)
        for (int i = 0; i < kCoefs; i += 2) {
            const float ta = (a - y_[i]) * kC[i] + x_[i];
            const float tb = (b - y_[i + 1]) * kC[i + 1] + x_[i + 1];
            x_[i] = a;
            x_[i + 1] = b;
            y_[i] = a = ta;
            y_[i + 1] = b = tb;
        }
        return 0.5f * (a + b);
    }

    void reset() {
        for (int i = 0; i < kCoefs; ++i) x_[i] = y_[i] = 0.0f;
    }

private:
    static constexpr float kC[kCoefs] = {0.0536154666f, 0.1934367242f, 0.3723159713f, 0.5466893949f,
                                         0.6930302670f, 0.8067034095f, 0.8940130551f, 0.9658743028f};
    float x_[kCoefs] = {}, y_[kCoefs] = {};
};

} // namespace sf
