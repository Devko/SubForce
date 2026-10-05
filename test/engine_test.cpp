// The engine's DSP, measured: the math helpers' error bounds, the decimator's response, the
// oscillators' aliasing, sync and the sub, the ladder (self-oscillation, slopes, bass loss),
// the envelopes' timing, idling, and stability at the extremes.
#include "host.h"
#include "../dsp/synth.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sft {
namespace {

using sf::Patch;
using sf::Synth;

// A patch that shows the oscillators as they are: filter wide open, no envelope on it, flat
// amp envelope, no drift, no velocity.
Patch plain() {
    Patch p;
    p.cutoffHz = 20000.0f;
    p.envAmount = 0.0f;
    p.keyTrack = 0.0f;
    p.drift = 0.0f;
    p.drive = 0.0f;
    p.aenv.attack = 0.001f;
    p.aenv.sustain = 1.0f;
    p.aenv.vel = 0.0f;
    p.fenv.vel = 0.0f;
    return p;
}

// Plays `note` and renders `skip` samples, then returns the next `n`.
std::vector<float> play(const Patch& p, int note, size_t n, size_t skip = 11025, int vel = 127) {
    Synth s;
    s.setPatch(p);
    s.noteOn(note, vel);
    std::vector<float> L(n + skip), R(n + skip);
    for (size_t b = 0; b < L.size(); b += 128) {
        const int m = static_cast<int>(std::min<size_t>(128, L.size() - b));
        s.render(&L[b], &R[b], m);
    }
    return std::vector<float>(L.begin() + static_cast<long>(skip), L.end());
}

double noteHzD(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

void testMath() {
    std::printf("== fast math\n");
    double e2 = 0, et = 0, eh = 0, eh12 = 0;
    for (int i = -2000; i <= 2000; ++i) {
        const float x = static_cast<float>(i) * 0.01f;
        e2 = std::max(e2, std::fabs(sf::exp2Fast(x) / std::exp2(static_cast<double>(x)) - 1.0));
    }
    for (int i = 1; i < 1000; ++i) {   // the ladder's range: cutoff up to 0.4 of the 2x rate
        const float w = 0.4f * sf::kPi * static_cast<float>(i) / 1000.0f;
        et = std::max(et, std::fabs(sf::tanFast(w) / std::tan(static_cast<double>(w)) - 1.0));
    }
    for (int i = 1; i <= 1200; ++i) {
        const double x = i * 0.01;
        const double err = std::fabs(sf::tanhXdX(static_cast<float>(x)) / (std::tanh(x) / x) - 1.0);
        if (x <= 5.0) eh = std::max(eh, err);
        else eh12 = std::max(eh12, err);
    }
    std::printf("  exp2Fast %.1e, tanFast %.1e, tanhXdX %.1e to 5, %.2f to 12\n", e2, et, eh, eh12);
    CHECK(e2 < 2e-7 && et < 5e-7 && eh < 0.01 && eh12 < 0.2);
    CHECK(sf::softclip(10.0f) == 1.0f && sf::softclip(-10.0f) == -1.0f && std::fabs(sf::softclip(0.1f) - std::tanh(0.1f)) < 1e-4);
    // log2Fast over many octaves; the reciprocals (NEON's estimate + Newton-Raphson on the device).
    double el = 0, er1 = 0, er2 = 0;
    for (int i = 0; i <= 4000; ++i) {
        const float x = std::pow(2.0f, -10.0f + static_cast<float>(i) * 0.005f);
        el = std::max(el, std::fabs(sf::log2Fast(x) - std::log2(static_cast<double>(x))));
        er2 = std::max(er2, std::fabs(static_cast<double>(sf::recip1<2>(x)) * x - 1.0));
        er1 = std::max(er1, std::fabs(static_cast<double>(sf::recip4<1>(sf::splat(x))[2]) * x - 1.0));
    }
    std::printf("  log2Fast %.1e, reciprocal %.1e (two steps), %.1e (one)\n", el, er2, er1);
    CHECK(el < 3e-6 && er2 < 1e-6 && er1 < 5e-5);
    // The four-lane versions against the scalar ones (the same polynomials).
    double d2 = 0, dt = 0, dh = 0;
    for (int i = 0; i < 1000; i += 4) {
        float x[4], w[4], h[4];
        for (int k = 0; k < 4; ++k) {
            x[k] = -20.0f + 0.04f * static_cast<float>(i + k);
            w[k] = 1.5f * static_cast<float>(i + k) / 1000.0f;
            h[k] = -12.0f + 0.024f * static_cast<float>(i + k);
        }
        const sf::f4 e = sf::exp2Fast4(sf::load4(x)), t = sf::tanFast4(sf::load4(w)), th = sf::tanhXdX4(sf::load4(h));
        for (int k = 0; k < 4; ++k) {
            d2 = std::max(d2, static_cast<double>(std::fabs(e[k] / sf::exp2Fast(x[k]) - 1.0f)));
            if (w[k] > 0.0f) dt = std::max(dt, static_cast<double>(std::fabs(t[k] / sf::tanFast(w[k]) - 1.0f)));
            dh = std::max(dh, static_cast<double>(std::fabs(th[k] / sf::tanhXdX(h[k]) - 1.0f)));
        }
    }
    std::printf("  four lanes against one: exp2 %.1e, tan %.1e, tanh(x)/x %.1e\n", d2, dt, dh);
    CHECK(d2 < 1e-6 && dt < 1e-6 && dh < 5e-5);
}

void testDecimator() {
    std::printf("== 2x decimator\n");
    // A sine at the high rate: gain through the decimator after it settles.
    auto gain = [](double hz) {
        sf::Decimator d;
        double peak = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const float a = static_cast<float>(std::sin(2 * M_PI * hz * (2 * i) / 88200.0));
            const float b = static_cast<float>(std::sin(2 * M_PI * hz * (2 * i + 1) / 88200.0));
            const float y = d.process(a, b);
            if (i > 4000) peak = std::max(peak, static_cast<double>(std::fabs(y)));
        }
        return 20.0 * std::log10(std::max(peak, 1e-12));
    };
    double pass = 0.0, stop = -300.0;
    for (double hz : {100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0, 20000.0}) pass = std::max(pass, std::fabs(gain(hz)));
    for (double hz : {24200.0, 26000.0, 30000.0, 35000.0, 40000.0, 44000.0}) stop = std::max(stop, gain(hz));
    std::printf("  passband to 20 kHz within %.3f dB, stopband from 24.2 kHz at %.1f dB\n", pass, stop);
    CHECK(pass < 0.01 && stop < -80.0);
}

void testOscillators() {
    std::printf("== oscillators: wave shapes, aliasing\n");
    const Patch p0 = plain();
    double worstAll = -300.0;
    for (float w : {0.0f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.85f, 1.0f})
        for (int note : {60, 84, 96}) {
            Patch p = p0;
            p.osc[0].wave = w;
            const auto x = play(p, note, 32768);
            const double a = worstAliasDb(x, noteHzD(note));
            worstAll = std::max(worstAll, a);
            const double pitch = pitchHz(x);
            if (w < 0.9f) CHECK(std::fabs(pitch / noteHzD(note) - 1.0) < 0.002);   // a narrow pulse crosses zero twice
            if (a > -50.0) std::printf("  wave %.2f note %d: worst alias %.1f dB\n", w, note, a);
            CHECK(a < -50.0);
        }
    // The oscillator itself has no DC at any shape (the pulse's mean is taken out, as an analog
    // output's coupling capacitor would): the engine's output DC blocker would hide it.
    for (float w : {0.0f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 1.0f}) {
        sf::Osc o;
        const sf::Shape sh = sf::shapeOf(w);
        bool wr = false;
        float wx = 0.0f;
        double mean = 0.0;
        const int n = 88200;   // a second at 100 Hz: whole cycles
        for (int i = 0; i < n; ++i) mean += o.tick(100.0f / 88200.0f, sh, wr, wx);
        CHECK(std::fabs(mean / n) < 0.005);
    }
    // A moving pulse width (a bus sweeping the wave between square and the narrow pulse): every
    // edge is corrected, the ones the width sweeps past too. Uncorrected, a step of 2 shows up
    // between two samples.
    for (float hz : {41.0f, 110.0f, 440.0f}) {
        sf::Osc o;
        float prev = 0.0f, worst = 0.0f;
        bool wr = false;
        float wx = 0.0f;
        for (int i = 0; i < 88200 * 2; ++i) {
            const float lfo = 0.5f + 0.5f * sf::sinCycle(std::fmod(5.0f * static_cast<float>(i) / 88200.0f, 1.0f));
            const float v = o.tick(hz / 88200.0f, sf::shapeOf(2.0f / 3.0f + lfo / 3.0f), wr, wx);
            if (i > 0) worst = std::max(worst, std::fabs(v - prev));
            prev = v;
        }
        std::printf("  pulse width swept at %.0f Hz: largest step between samples %.2f\n", hz, worst);
        CHECK(worst < 1.6f);
    }
    // The sub switched between -1 and -2 octaves mid-note: a band-limited step too.
    {
        sf::Sub sub;
        float prev = 0.0f, worst = 0.0f;
        for (int i = 0; i < 88200; ++i) {
            const bool wrapped = i % 200 == 150;   // osc 1 at 441 Hz, wrapping between samples
            const float v = sub.tick(wrapped, 0.5f, (i / 3001) % 2 ? 2 : 1);
            if (i > 0) worst = std::max(worst, std::fabs(v - prev));
            prev = v;
        }
        CHECK(worst < 1.6f);
    }
    std::printf("  worst alias over the shapes at C4..C7: %.1f dB\n", worstAll);
    // Same oscillator through the shapes: triangle the quietest, square the loudest.
    Patch t = p0, q = p0;
    t.osc[0].wave = 0.0f;
    q.osc[0].wave = 2.0f / 3.0f;
    CHECK(rms(play(t, 48, 8192)) < rms(play(q, 48, 8192)));

    std::printf("== hard sync, sub oscillator, keyboard reset\n");
    for (float semis : {5.3f, 7.0f, -3.0f}) {
        Patch s = p0;
        s.sync = true;
        s.mixOsc1 = 0.0f;
        s.mixOsc2 = 0.8f;
        s.osc[1].octave = 1;
        s.osc2Semis = semis;
        const auto x = play(s, 72, 32768);
        const double a = worstAliasDb(x, noteHzD(72));   // synced: periodic at the master's pitch
        std::printf("  sync +%.1f st: worst alias %.1f dB\n", 12 + semis, a);
        CHECK(a < -45.0);
        if (semis != 7.0f) {   // +19 st is almost exactly 3x: unsynced, it would pass as periodic
            Patch u = s;
            u.sync = false;
            CHECK(worstAliasDb(play(u, 72, 32768), noteHzD(72)) > -20.0);   // unsynced it isn't
        }
    }
    for (int oct : {sf::SO_ONE, sf::SO_TWO}) {
        Patch s = p0;
        s.mixOsc1 = 0.0f;
        s.mixSub = 0.8f;
        s.subOctave = oct;
        const auto x = play(s, 72, 32768);
        const double want = noteHzD(72 - 12 * (oct + 1));
        CHECK(std::fabs(pitchHz(x) / want - 1.0) < 0.002);
        CHECK(worstAliasDb(x, want) < -50.0);
    }
    // Keyboard reset: two notes start on the same waveform; free running they don't.
    for (bool reset : {true, false}) {
        Patch r = p0;
        r.kbReset = reset;
        r.aenv.attack = 0.001f;
        Synth s;
        s.setPatch(r);
        std::vector<float> a(512), b(512), junk(4410);
        s.noteOn(60, 127);
        s.render(a.data(), junk.data(), 512);
        s.noteOff(60);
        for (int i = 0; i < 20; ++i) s.render(junk.data(), junk.data(), 4410);   // silent again
        s.render(junk.data(), junk.data(), 37);   // and some odd number of samples later
        s.noteOn(60, 127);
        s.render(b.data(), junk.data(), 512);
        double d = 0.0;
        for (int i = 0; i < 512; ++i) d = std::max(d, static_cast<double>(std::fabs(a[i] - b[i])));
        CHECK(reset ? d < 0.02 : d > 0.05);
    }
}

void testLadder() {
    std::printf("== ladder: self-oscillation, slopes, bass loss, key track\n");
    Patch p = plain();
    p.mixOsc1 = 0.0f;
    p.res = 1.0f;
    for (float hz : {110.0f, 440.0f, 1760.0f, 7040.0f}) {
        p.cutoffHz = hz;
        const auto x = play(p, 60, 22050, 44100);
        const double f = pitchHz(x), cents = 1200.0 * std::log2(f / hz);
        std::printf("  resonance 100%%, cutoff %5.0f Hz: oscillates at %.1f Hz (%+.0f ct), rms %.3f\n", hz, f, cents, rms(x));
        CHECK(std::fabs(cents) < 60.0 && rms(x) > 0.03 && rms(x) < 0.5);
    }
    p.res = 0.8f;   // below the edge: no oscillation of its own
    p.cutoffHz = 1000.0f;
    CHECK(rms(play(p, 60, 22050, 44100)) < 0.001);
    // Key track 100%: the oscillation follows the keyboard.
    p.res = 1.0f;
    p.keyTrack = 1.0f;
    p.cutoffHz = 440.0f;
    const double f60 = pitchHz(play(p, 60, 22050, 44100)), f72 = pitchHz(play(p, 72, 22050, 44100));
    CHECK(std::fabs(f72 / f60 - 2.0) < 0.04);

    // Slopes: a triangle (soft harmonics) an octave and two above a 400 Hz cutoff.
    Patch s = plain();
    s.osc[0].wave = 0.0f;
    s.cutoffHz = 400.0f;
    for (int slope = sf::SL_6; slope <= sf::SL_24; ++slope) {
        s.slope = slope;
        const double lo = rms(play(s, 45, 16384)), mid = rms(play(s, 81, 16384)), hi = rms(play(s, 93, 16384));
        const double perOct = 20.0 * std::log10(mid / hi);
        std::printf("  %2d dB slope: %.1f dB at 880 Hz, %.1f dB/oct from 880 Hz to 1.76 kHz\n", 6 * (slope + 1), 20 * std::log10(mid / lo), perOct);
        CHECK(std::fabs(perOct - 6.0 * (slope + 1)) < 3.5);
    }
    // Resonance thins the bass (the ladder's 1 / (1 + r) passband).
    s.slope = sf::SL_24;
    s.cutoffHz = 3000.0f;
    s.res = 0.0f;
    const double clean = rms(play(s, 45, 16384));
    s.res = 0.85f;
    const double thin = rms(play(s, 45, 16384));
    CHECK(20 * std::log10(clean / thin) > 8.0);
    // Multidrive: louder and denser, never more than a few dB.
    Patch d = plain();
    d.cutoffHz = 1500.0f;
    d.drive = 0.0f;
    const double d0 = rms(play(d, 36, 16384));
    d.drive = 1.0f;
    const double d1 = rms(play(d, 36, 16384));
    std::printf("  Multidrive 0 -> 100%%: %+.1f dB\n", 20 * std::log10(d1 / d0));
    CHECK(d1 > d0 && 20 * std::log10(d1 / d0) < 9.0);
}

void testEnvelopes() {
    std::printf("== envelopes\n");
    const float sr = 44100.0f;
    sf::EnvTimes t;
    t.delay = 0.01f;
    t.attack = 0.02f;
    t.hold = 0.03f;
    t.decay = 0.1f;
    t.sustain = 0.5f;
    t.release = 0.2f;
    const sf::EnvCoef c = sf::envCoef(t, sr, 1.0f);
    sf::Env e;
    e.trigger(c, true);
    int i = 0, attackAt = -1, holdEnd = -1;
    bool quiet = true;
    for (; i < 44100; ++i) {
        const float v = e.tick(c);
        if (i < 440) quiet = quiet && v == 0.0f;             // delay
        if (attackAt < 0 && v >= 1.0f) attackAt = i;
        if (attackAt >= 0 && holdEnd < 0 && v < 1.0f) holdEnd = i;
        if (i == 22050) CHECK(std::fabs(v - 0.5f) < 0.001f);  // decayed onto the sustain
    }
    CHECK(quiet);
    std::printf("  attack peaks at %.1f ms (delay 10 + attack 20), hold ends at %.1f ms (+30)\n", attackAt / 44.1, holdEnd / 44.1);
    CHECK(std::abs(attackAt - 1323) < 45 && std::abs(holdEnd - attackAt - 1323) < 10);
    e.release();
    int dead = 0;
    float v = 0.0f;
    for (int k = 0; k < 44100; ++k) {
        v = e.tick(c);
        if (v <= 0.5f * 0.001f && dead == 0) dead = k;   // 60 dB under the sustain
    }
    CHECK(std::abs(dead - 8820) < 100 && v == 0.0f && e.stage == sf::E_IDLE);
    // Loop: rounds again while held.
    sf::EnvTimes lt = t;
    lt.delay = lt.hold = 0.0f;
    lt.loop = true;
    const sf::EnvCoef lc = sf::envCoef(lt, sr, 1.0f);
    sf::Env l;
    l.trigger(lc, true);
    int peaks = 0;
    float prev = 0.0f;
    for (int k = 0; k < 44100; ++k) {
        const float x = l.tick(lc);
        if (x >= 1.0f && prev < 1.0f) ++peaks;
        prev = x;
    }
    std::printf("  loop: %d peaks in a second\n", peaks);
    CHECK(peaks >= 3);
    // Retrigger: Reset starts from 0, else from where it is.
    sf::Env r;
    r.trigger(c, true);
    for (int k = 0; k < 4000; ++k) r.tick(c);
    const float at = r.v;
    r.trigger(c, false);
    CHECK(r.v == at);
    r.trigger(c, true);
    CHECK(r.v == 0.0f);
    // Keyboard tracking: an octave up, half the time.
    const sf::EnvCoef up = sf::envCoef(t, sr, 0.5f);
    CHECK(std::abs(2 * up.delayN - c.delayN) <= 1 && up.att > c.att);
}

void testIdleAndStability() {
    std::printf("== idle, stability\n");
    Synth s;
    Patch p;
    p.aenv.release = 0.05f;
    s.setPatch(p);
    std::vector<float> L(4410), R(4410);
    CHECK(s.info().silent);
    s.noteOn(48, 100);
    s.render(L.data(), R.data(), 4410);
    CHECK(!s.info().silent && s.activeVoices() == 1);
    s.noteOff(48);
    for (int k = 0; k < 10; ++k) s.render(L.data(), R.data(), 4410);
    CHECK(s.info().silent && s.activeVoices() == 0);
    bool zero = true;
    for (float v : L) zero = zero && v == 0.0f;
    CHECK(zero);

    // Everything up: both oscillators, sub, noise, feedback, sync, full drive and resonance,
    // the cutoff swept by a full envelope and a fast bus. Bounded and finite.
    Patch x;
    x.mixOsc1 = x.mixOsc2 = x.mixSub = x.mixNoise = x.mixFeedback = 1.0f;
    x.sync = true;
    x.res = x.drive = 1.0f;
    x.envAmount = 1.0f;
    x.fenv.attack = 0.001f;
    x.fenv.decay = 0.05f;
    x.fenv.loop = true;
    x.mod[0].src = sf::MS_SQUARE;
    x.mod[0].rateHz = 100.0f;
    x.mod[0].filter = 1.0f;
    x.mod[0].pitch = 1.0f;
    x.mod[0].dest = sf::MD_FEEDBACK;
    x.mod[0].amount = 1.0f;
    float peak = 0.0f;
    bool finite = true;
    for (int slope = 0; slope < 4; ++slope)
        for (int note : {12, 48, 96, 127}) {
            x.slope = slope;
            Synth y;
            y.setPatch(x);
            y.noteOn(note, 127);
            for (int k = 0; k < 10; ++k) {
                y.render(L.data(), R.data(), 4410);
                for (float v : L) {
                    finite = finite && std::isfinite(v);
                    peak = std::max(peak, std::fabs(v));
                }
            }
        }
    std::printf("  everything at full: peak %.2f\n", peak);
    CHECK(finite && peak < 1.5f);

    // Velocity and the amp EG: at 100% velocity amount, a quarter of the velocity is a quarter of
    // the level (-12 dB).
    Patch v;
    v.cutoffHz = 20000.0f;
    v.envAmount = 0.0f;
    v.drift = 0.0f;
    v.aenv.sustain = 1.0f;
    v.aenv.vel = 1.0f;
    Synth hard, soft;
    hard.setPatch(v);
    soft.setPatch(v);
    hard.noteOn(48, 127);
    soft.noteOn(48, 32);
    std::vector<float> a(22050), b(22050), junk(22050);
    hard.render(a.data(), junk.data(), 22050);
    soft.render(b.data(), junk.data(), 22050);
    CHECK(std::fabs(20.0 * std::log10(rms(a, 11025) / rms(b, 11025)) - 20.0 * std::log10(127.0 / 32.0)) < 0.5);
    // Noise: about the same loudness at every colour.
    double lo = 1e9, hi = 0.0;
    for (float c : {0.0f, 0.25f, 0.5f, 1.0f}) {
        Patch n = plain();
        n.mixOsc1 = 0.0f;
        n.mixNoise = 0.8f;
        n.noiseColor = c;
        const double r = rms(play(n, 60, 22050));
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    CHECK(20.0 * std::log10(hi / lo) < 2.0);
}

} // namespace

void engineTests() {
    testMath();
    testDecimator();
    testOscillators();
    testLadder();
    testEnvelopes();
    testIdleAndStability();
}

} // namespace sft
