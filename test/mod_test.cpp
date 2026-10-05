// The two mod busses: shapes and depth on pitch, rate (free, synced, locked to the bar),
// what controls the depth (mod wheel, velocity), retrigger, the filter EG as a source, the
// programmable destination (volume, wave, the other bus's rate).
#include "host.h"
#include "../dsp/synth.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sft {
namespace {

using sf::Patch;
using sf::Synth;

Patch base() {
    Patch p;
    p.cutoffHz = 20000.0f;
    p.envAmount = 0.0f;
    p.keyTrack = 0.0f;
    p.drift = 0.0f;
    p.aenv.attack = 0.001f;
    p.aenv.sustain = 1.0f;
    p.aenv.vel = 0.0f;
    p.fenv.vel = 0.0f;
    p.mod[0].control = sf::MC_ALWAYS;
    p.mod[1].control = sf::MC_ALWAYS;
    return p;
}

// One semitone from a bus's pitch amount (a * |a| * 24).
const float kOneSemi = std::sqrt(1.0f / 24.0f);

std::vector<float> render(Synth& s, int samples) {
    std::vector<float> L(static_cast<size_t>(samples)), R(L.size());
    for (int b = 0; b < samples; b += 128) s.render(&L[static_cast<size_t>(b)], &R[static_cast<size_t>(b)], std::min(128, samples - b));
    return L;
}

// Pitch in semitones against A3 (220 Hz) between t0 and t1 seconds of x.
double semis(const std::vector<float>& x, double t0, double t1) {
    return 12.0 * std::log2(pitchHz(x, static_cast<size_t>(t0 * 44100), static_cast<size_t>(t1 * 44100)) / 220.0);
}

void testShapesAndRate() {
    std::printf("== mod busses: depth, rate, sync\n");
    // A 1 Hz square, one semitone: half a second up, half down.
    Patch p = base();
    p.mod[0].src = sf::MS_SQUARE;
    p.mod[0].rateHz = 1.0f;
    p.mod[0].pitch = kOneSemi;
    p.mod[0].retrig = true;
    Synth s;
    s.setPatch(p);
    s.noteOn(57, 100);
    const auto x = render(s, 44100 * 2);
    const double up = semis(x, 0.05, 0.45), down = semis(x, 0.55, 0.95), up2 = semis(x, 1.05, 1.45);
    std::printf("  square 1 Hz, 1 st: %+.2f / %+.2f / %+.2f st\n", up, down, up2);
    CHECK(std::fabs(up - 1.0) < 0.03 && std::fabs(down + 1.0) < 0.03 && std::fabs(up2 - 1.0) < 0.03);

    // Pitch dest Osc 2 only: osc 1 stays put.
    p.mod[0].pitchDest = sf::OD_OSC2;
    Synth o;
    o.setPatch(p);
    o.noteOn(57, 100);
    CHECK(std::fabs(semis(render(o, 22050), 0.05, 0.45)) < 0.02);

    // Synced at 120 BPM, 1/4 = 2 Hz: a quarter second each way.
    p = base();
    p.mod[0].src = sf::MS_SQUARE;
    p.mod[0].sync = true;
    p.mod[0].div = 6;   // 1/4
    p.mod[0].pitch = kOneSemi;
    p.mod[0].retrig = true;
    Synth y;
    y.setPatch(p);
    y.setTransport(120.0, 0.0, false, false);
    y.noteOn(57, 100);
    const auto z = render(y, 44100);
    CHECK(semis(z, 0.03, 0.22) > 0.95 && semis(z, 0.28, 0.47) < -0.95 && semis(z, 0.53, 0.72) > 0.95);

    // Free-running synced bus with the transport playing: locked to the beat, whenever the note comes.
    p.mod[0].retrig = false;
    Synth k;
    k.setPatch(p);
    k.setTransport(120.0, 10.5, true, true);   // half way through a beat: the square is down
    k.noteOn(57, 100);
    std::vector<float> L(4410), R(4410);
    k.render(L.data(), R.data(), 4410);
    CHECK(semis(L, 0.02, 0.1) < -0.95);
    k.setTransport(120.0, 11.0, true, true);   // on the beat: up
    k.render(L.data(), R.data(), 4410);
    CHECK(semis(L, 0.02, 0.1) > 0.95);
}

void testSources() {
    std::printf("== mod busses: every source\n");
    // Each source at 1 Hz on one semitone of pitch, retriggered: where it stands across the cycle.
    auto track = [](int src, double* out) {
        Patch p = base();
        p.mod[0].src = src;
        p.mod[0].rateHz = 1.0f;
        p.mod[0].pitch = kOneSemi;
        p.mod[0].retrig = true;
        Synth s;
        s.setPatch(p);
        s.noteOn(57, 100);
        const auto x = render(s, 44100);
        for (int k = 0; k < 4; ++k) out[k] = semis(x, 0.25 * k + 0.08, 0.25 * k + 0.17);   // around 1/8, 3/8, 5/8, 7/8
    };
    double v[4];
    track(sf::MS_TRIANGLE, v);   // 0 -> 1 -> 0 -> -1 -> 0
    CHECK(v[0] > 0.3 && v[1] > 0.3 && v[2] < -0.3 && v[3] < -0.3);
    track(sf::MS_SAW, v);        // falls from 1 to -1
    CHECK(v[0] > v[1] && v[1] > v[2] && v[2] > v[3] && v[0] > 0.5 && v[3] < -0.5);
    track(sf::MS_RAMP, v);       // rises from -1 to 1
    CHECK(v[0] < v[1] && v[1] < v[2] && v[2] < v[3] && v[0] < -0.5 && v[3] > 0.5);
    track(sf::MS_SAMPLE_HOLD, v);   // one value the whole cycle
    CHECK(std::fabs(v[0] - v[3]) < 0.02 && std::fabs(v[0]) <= 1.02);
    track(sf::MS_SMOOTH, v);     // moving, inside the range
    bool inside = true;
    for (double x : v) inside = inside && std::fabs(x) <= 1.02;
    CHECK(inside);
    // Eight cycles at 4 Hz: a new random value every cycle (not one stuck value), held across
    // it (S&H) or glided to monotonically within it (Smooth).
    auto cycles = [](int src, double* early, double* mid, double* late) {
        Patch p = base();
        p.mod[0].src = src;
        p.mod[0].rateHz = 4.0f;
        p.mod[0].pitch = kOneSemi;
        p.mod[0].retrig = true;
        Synth s;
        s.setPatch(p);
        s.noteOn(57, 100);
        const auto x = render(s, 2 * 44100);
        for (int c = 0; c < 8; ++c) {
            const double t = 0.25 * c;
            early[c] = semis(x, t + 0.03, t + 0.08);
            mid[c] = semis(x, t + 0.10, t + 0.15);
            late[c] = semis(x, t + 0.17, t + 0.22);
        }
    };
    double early[8], mid[8], late[8];
    cycles(sf::MS_SAMPLE_HOLD, early, mid, late);
    double lo = 9.0, hi = -9.0, held = 0.0;
    for (int c = 0; c < 8; ++c) {
        lo = std::min(lo, mid[c]);
        hi = std::max(hi, mid[c]);
        held = std::max(held, std::fabs(late[c] - early[c]));
    }
    std::printf("  S&H over 8 cycles: %.2f .. %.2f st, held within %.3f st\n", lo, hi, held);
    CHECK(hi - lo > 0.5 && held < 0.02 && lo >= -1.02 && hi <= 1.02);
    cycles(sf::MS_SMOOTH, early, mid, late);
    lo = 9.0, hi = -9.0;
    bool monotonic = true;
    for (int c = 0; c < 8; ++c) {
        lo = std::min(lo, mid[c]);
        hi = std::max(hi, mid[c]);
        monotonic = monotonic && (mid[c] - early[c]) * (late[c] - mid[c]) >= -1e-4;
    }
    std::printf("  Smooth over 8 cycles: %.2f .. %.2f st\n", lo, hi);
    CHECK(hi - lo > 0.3 && monotonic && lo >= -1.02 && hi <= 1.02);
}

void testControl() {
    std::printf("== mod busses: control, retrigger, filter EG source\n");
    // Mod wheel: no wheel, no modulation; full wheel, full depth.
    Patch p = base();
    p.mod[0].src = sf::MS_SQUARE;
    p.mod[0].rateHz = 1.0f;
    p.mod[0].pitch = kOneSemi;
    p.mod[0].retrig = true;
    p.mod[0].control = sf::MC_MODWHEEL;
    Synth s;
    s.setPatch(p);
    s.noteOn(57, 100);
    CHECK(std::fabs(semis(render(s, 22050), 0.05, 0.45)) < 0.02);
    s.controller(1, 127);
    s.noteOff(57);
    s.noteOn(57, 100);   // retriggered: up again
    CHECK(std::fabs(semis(render(s, 22050), 0.05, 0.45) - 1.0) < 0.03);
    // Velocity: half the velocity, half the depth.
    p.mod[0].control = sf::MC_VELOCITY;
    Synth v;
    v.setPatch(p);
    v.noteOn(57, 64);
    CHECK(std::fabs(semis(render(v, 22050), 0.05, 0.45) - 64.0 / 127.0) < 0.03);
    // Aftertouch.
    p.mod[0].control = sf::MC_AFTERTOUCH;
    Synth a;
    a.setPatch(p);
    a.noteOn(57, 100);
    a.aftertouch(1.0f);
    CHECK(std::fabs(semis(render(a, 22050), 0.05, 0.45) - 1.0) < 0.03);

    // The filter EG as a source: a slow attack bends the pitch up an octave.
    Patch f = base();
    f.mod[0].src = sf::MS_FILTER_EG;
    f.mod[0].pitch = std::sqrt(0.5f);   // 12 st
    f.fenv.attack = 0.5f;
    f.fenv.sustain = 1.0f;
    Synth e;
    e.setPatch(f);
    e.noteOn(45, 100);
    const auto x = render(e, 44100);
    CHECK(semis(x, 0.0, 0.05) < -10.0 && std::fabs(semis(x, 0.8, 0.95)) < 0.1);
}

// A 1 Hz square on `dest` (amount `amt`): the rms of its up half and its down half.
void halves(const Patch& p0, int dest, float amt, double& up, double& down) {
    Patch p = p0;
    p.mod[0].src = sf::MS_SQUARE;
    p.mod[0].rateHz = 1.0f;
    p.mod[0].dest = dest;
    p.mod[0].amount = amt;
    p.mod[0].retrig = true;
    Synth s;
    s.setPatch(p);
    s.noteOn(45, 100);
    const auto x = render(s, 44100);
    up = rms(x, 4410, 19845);
    down = rms(x, 26460, 41895);
}

void testDestinations() {
    std::printf("== mod busses: destinations\n");
    {
        double up = 0.0, down = 0.0;
        Patch p = base();
        p.cutoffHz = 800.0f;
        halves(p, sf::MD_RES, 0.8f, up, down);      // resonance thins the bass: different levels
        CHECK(std::fabs(20.0 * std::log10(up / down)) > 2.0);
        halves(p, sf::MD_DRIVE, 1.0f, up, down);    // driven: louder
        CHECK(up > down * 1.1);
        Patch q = base();
        q.mixOsc1 = 0.0f;
        halves(q, sf::MD_SUB, 1.0f, up, down);      // the sub only while the bus is up
        CHECK(up > 0.05 && down < 1e-3);
        halves(q, sf::MD_NOISE, 1.0f, up, down);
        CHECK(up > 0.05 && down < 1e-3);
        halves(p, sf::MD_FEEDBACK, 1.0f, up, down); // feedback changes the sound
        CHECK(std::fabs(20.0 * std::log10(up / down)) > 0.5);
        Patch w = base();
        w.mixOsc1 = 0.0f;
        w.mixOsc2 = 0.8f;
        w.osc[1].wave = 0.0f;
        halves(w, sf::MD_WAVE2, 2.0f / 3.0f, up, down);   // osc 2: square up, triangle down
        CHECK(up > 1.3 * down);
        halves(w, sf::MD_WAVE1, 2.0f / 3.0f, up, down);   // osc 1's wave: osc 2 doesn't hear it
        CHECK(std::fabs(up / down - 1.0) < 0.05);
        halves(w, sf::MD_WAVE, 2.0f / 3.0f, up, down);    // both
        CHECK(up > 1.3 * down);
    }
    // Volume: a square at full amount: silent half the time, twice as loud the other.
    Patch p = base();
    p.mod[0].src = sf::MS_SQUARE;
    p.mod[0].rateHz = 1.0f;
    p.mod[0].dest = sf::MD_VOLUME;
    p.mod[0].amount = -1.0f;
    p.mod[0].retrig = true;
    Synth s;
    s.setPatch(p);
    s.noteOn(57, 100);
    const auto x = render(s, 44100);
    CHECK(rms(x, 4410, 19845) < 1e-3 && rms(x, 26460, 41895) > 0.05);
    // Wave: the shape moves (triangle to square changes the level).
    p.mod[0].dest = sf::MD_WAVE1;
    p.mod[0].amount = 2.0f / 3.0f;
    p.osc[0].wave = 0.0f;
    p.mod[0].src = sf::MS_RAMP;   // -1 .. 1 over the cycle: triangle at the start, square at the end
    Synth w;
    w.setPatch(p);
    w.noteOn(57, 100);
    const auto y = render(w, 44100);
    CHECK(rms(y, 39690, 44100) > 1.3 * rms(y, 22050, 26460));
    // Other Rate: bus 1 (the filter EG, held at 1) doubles bus 2's rate.
    Patch r = base();
    r.fenv.attack = 0.001f;
    r.fenv.sustain = 1.0f;
    r.mod[0].src = sf::MS_FILTER_EG;
    r.mod[0].dest = sf::MD_OTHER_RATE;
    r.mod[0].amount = 0.25f;   // 2^(4 * 0.25) = x2
    r.mod[1].src = sf::MS_SQUARE;
    r.mod[1].rateHz = 1.0f;
    r.mod[1].pitch = kOneSemi;
    r.mod[1].retrig = true;
    Synth o;
    o.setPatch(r);
    o.noteOn(57, 100);
    const auto z = render(o, 44100);
    CHECK(semis(z, 0.03, 0.22) > 0.95 && semis(z, 0.28, 0.47) < -0.95);   // 2 Hz now
    // ...and so it does on a synced bus while MPC plays (which would otherwise lock to the bar).
    r.mod[1].sync = true;
    r.mod[1].div = 3;   // 1 bar: 0.5 Hz at 120 BPM, 1 Hz doubled
    r.mod[1].retrig = false;
    Synth k;
    k.setPatch(r);
    k.setTransport(120.0, 0.0, true, true);
    k.noteOn(57, 100);
    const auto u = render(k, 44100);
    CHECK(semis(u, 0.05, 0.45) > 0.95 && semis(u, 0.55, 0.95) < -0.95);
}

} // namespace

void modTests() {
    testShapesAndRate();
    testSources();
    testControl();
    testDestinations();
}

} // namespace sft
