// The two mod busses: shapes and depth on pitch, rate (free, synced, locked to the bar, Hi range),
// what controls the depth (mod wheel, velocity, the depth amounts), retrigger, every source, the
// programmable destination; osc 2's beat frequency; the plugin side of it.
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

// The pitch a bus gives one note (one semitone at full: kOneSemi), in semitones against the note.
double busSemis(Patch p, int note, int vel = 100, float wheel = 0.0f, float pressure = 0.0f) {
    p.mod[0].pitch = kOneSemi;
    Synth s;
    s.setPatch(p);
    if (wheel > 0.0f) s.controller(1, static_cast<int>(wheel * 127.0f + 0.5f));
    s.noteOn(note, vel);
    if (pressure > 0.0f) s.aftertouch(pressure);
    const auto x = render(s, 22050);
    return 12.0 * std::log2(pitchHz(x, 2205, 22050) / (440.0 * std::pow(2.0, (note - 69) / 12.0)));
}

void testBeat() {
    std::printf("== osc 2 beat frequency\n");
    // Osc 2 alone, +3.5 Hz: as many Hz above the key low and high (not as many cents).
    Patch p = base();
    p.mixOsc1 = 0.0f;
    p.mixOsc2 = 0.8f;
    p.beatHz = 3.5f;
    for (int note : {45, 69}) {
        Synth s;
        s.setPatch(p);
        s.noteOn(note, 100);
        const double off = pitchHz(render(s, 44100), 4410, 44100) - 440.0 * std::pow(2.0, (note - 69) / 12.0);
        std::printf("  note %d: %+.2f Hz\n", note, off);
        CHECK(std::fabs(off - 3.5) < 0.05);
    }
    // From a bus: Constant at -100% on Beat Freq takes it 3.5 Hz the other way.
    p.beatHz = 0.0f;
    p.mod[0].src = sf::MS_CONSTANT;
    p.mod[0].dest = sf::MD_BEAT;
    p.mod[0].amount = -1.0f;
    Synth s;
    s.setPatch(p);
    s.noteOn(57, 100);
    CHECK(std::fabs(pitchHz(render(s, 44100), 4410, 44100) - (220.0 - 3.5)) < 0.05);
}

void testMoreSources() {
    std::printf("== mod busses: sine, noise, amp EG, velocity, aftertouch, key, constant\n");
    // Sine at 1 Hz: like the triangle, rounder (around 1/8 of the cycle: 0.70, the triangle 0.5).
    Patch p = base();
    p.mod[0].src = sf::MS_SINE;
    p.mod[0].rateHz = 1.0f;
    p.mod[0].pitch = kOneSemi;
    p.mod[0].retrig = true;
    Synth s;
    s.setPatch(p);
    s.noteOn(57, 100);
    const auto x = render(s, 44100);
    double v[4];
    for (int k = 0; k < 4; ++k) v[k] = semis(x, 0.25 * k + 0.08, 0.25 * k + 0.17);
    std::printf("  sine: %+.2f %+.2f %+.2f %+.2f st\n", v[0], v[1], v[2], v[3]);
    CHECK(std::fabs(v[0] - 0.70) < 0.06 && std::fabs(v[1] - 0.70) < 0.06 && std::fabs(v[2] + 0.70) < 0.06 &&
          std::fabs(v[3] + 0.70) < 0.06);

    // Noise: random within -1..1, and the rate is its speed: 50 ms apart, slow noise (0.2 Hz) is
    // still nearly where it was, fast noise (20 Hz) somewhere else (the windows' correlation).
    auto noise = [](float rate, double& spread, double& corr) {
        Patch q = base();
        q.mod[0].src = sf::MS_NOISE;
        q.mod[0].rateHz = rate;
        q.mod[0].pitch = kOneSemi;
        Synth n;
        n.setPatch(q);
        n.noteOn(57, 100);
        const auto y = render(n, 6 * 44100);
        std::vector<double> w(100);
        double lo = 9.0, hi = -9.0, mean = 0.0;
        for (size_t k = 0; k < w.size(); ++k) {
            w[k] = semis(y, 1.0 + 0.05 * static_cast<double>(k), 1.05 + 0.05 * static_cast<double>(k));
            lo = std::min(lo, w[k]);
            hi = std::max(hi, w[k]);
            mean += w[k] / static_cast<double>(w.size());
        }
        double c0 = 0.0, c1 = 0.0;
        for (size_t k = 0; k < w.size(); ++k) {
            c0 += (w[k] - mean) * (w[k] - mean);
            if (k > 0) c1 += (w[k] - mean) * (w[k - 1] - mean);
        }
        spread = hi - lo;
        corr = c1 / c0;
        CHECK(lo >= -1.02 && hi <= 1.02);
    };
    double spreadSlow = 0.0, corrSlow = 0.0, spreadFast = 0.0, corrFast = 0.0;
    noise(0.2f, spreadSlow, corrSlow);
    noise(20.0f, spreadFast, corrFast);
    std::printf("  noise 0.2 Hz: spread %.2f st, correlation %.2f; 20 Hz: %.2f st, %.2f\n", spreadSlow, corrSlow,
                spreadFast, corrFast);
    CHECK(spreadSlow > 0.3 && spreadFast > 0.3 && corrSlow > 0.7 && corrFast < 0.4);

    // The amp EG: a slow attack bends the pitch up an octave.
    Patch a = base();
    a.mod[0].src = sf::MS_AMP_EG;
    a.mod[0].pitch = std::sqrt(0.5f);   // 12 st
    a.aenv.attack = 0.5f;
    Synth e;
    e.setPatch(a);
    e.noteOn(45, 100);
    const auto y = render(e, 44100);
    CHECK(semis(y, 0.0, 0.05) < -10.0 && std::fabs(semis(y, 0.8, 0.95)) < 0.1);

    // Velocity, aftertouch, the key (two octaves above C3 is 1), and Constant.
    Patch c = base();
    c.mod[0].src = sf::MS_VELOCITY;
    CHECK(std::fabs(busSemis(c, 57, 64) - 64.0 / 127.0) < 0.03);
    c.mod[0].src = sf::MS_AFTERTOUCH;
    CHECK(std::fabs(busSemis(c, 57, 100, 0.0f, 0.5f) - 0.5) < 0.03);
    c.mod[0].src = sf::MS_KEY;
    CHECK(std::fabs(busSemis(c, 84) - 1.0) < 0.03 && std::fabs(busSemis(c, 48) + 0.5) < 0.03);
    c.mod[0].src = sf::MS_CONSTANT;
    CHECK(std::fabs(busSemis(c, 57) - 1.0) < 0.03);
}

void testDepthAmounts() {
    std::printf("== mod busses: depth amounts\n");
    Patch p = base();
    p.mod[0].src = sf::MS_CONSTANT;
    // None: only the amounts. Velocity +100%: the velocity is the depth.
    p.mod[0].control = sf::MC_NONE;
    CHECK(std::fabs(busSemis(p, 57)) < 0.02);
    p.mod[0].vel = 1.0f;
    CHECK(std::fabs(busSemis(p, 57, 64) - 64.0 / 127.0) < 0.03);
    // Always, the wheel at -100%: the wheel takes the bus away.
    p.mod[0].control = sf::MC_ALWAYS;
    p.mod[0].vel = 0.0f;
    p.mod[0].wheel = -1.0f;
    CHECK(std::fabs(busSemis(p, 57) - 1.0) < 0.03 && std::fabs(busSemis(p, 57, 100, 1.0f)) < 0.03);
    // Mod Wheel, pressure +50%: with the wheel down, pressing still brings in half.
    p.mod[0].control = sf::MC_MODWHEEL;
    p.mod[0].wheel = 0.0f;
    p.mod[0].at = 0.5f;
    CHECK(std::fabs(busSemis(p, 57, 100, 0.0f, 1.0f) - 0.5) < 0.03);
    // The depth stays within -1..1: Always and velocity +100% is still full, no more.
    p.mod[0].control = sf::MC_ALWAYS;
    p.mod[0].at = 0.0f;
    p.mod[0].vel = 1.0f;
    CHECK(std::fabs(busSemis(p, 57, 127) - 1.0) < 0.03);
}

// When the pitch a slow EG bends up an octave (filter EG: bus 2) is half-way (-6 st), with
// bus 1's Constant on `dest` at `amount`: seconds.
double halfWay(int dest, int egSource, float amount) {
    Patch p = base();
    p.fenv.attack = 0.5f;
    p.fenv.sustain = 1.0f;
    p.aenv.attack = 0.5f;
    p.mod[0].src = sf::MS_CONSTANT;
    p.mod[0].dest = dest;
    p.mod[0].amount = amount;
    p.mod[1].src = egSource;
    p.mod[1].pitch = std::sqrt(0.5f);   // 12 st
    Synth s;
    s.setPatch(p);
    s.noteOn(45, 100);
    const auto x = render(s, 2 * 44100);
    for (int w = 0; w < 80; ++w)
        if (semis(x, 0.025 * w, 0.025 * w + 0.025) > -6.0) return 0.025 * w + 0.0125;
    return 9.0;
}

void testMoreDestinations() {
    std::printf("== mod busses: more destinations\n");
    double up = 0.0, down = 0.0;
    Patch q = base();
    q.mixOsc1 = 0.0f;
    halves(q, sf::MD_OSC1, 1.0f, up, down);   // osc 1's level only while the bus is up
    CHECK(up > 0.05 && down < 1e-3);
    halves(q, sf::MD_OSC2, 1.0f, up, down);   // osc 2's
    CHECK(up > 0.05 && down < 1e-3);
    Patch f = base();
    f.cutoffHz = 200.0f;
    f.fenv.sustain = 1.0f;
    halves(f, sf::MD_EG_AMOUNT, 0.6f, up, down);   // the filter EG opens the filter only while up
    CHECK(up > 2.0 * down);
    Patch k = base();
    k.cutoffHz = 800.0f;
    halves(k, sf::MD_KEY_TRACK, 0.5f, up, down);   // 100% key tracking on A2: the filter closes
    CHECK(20.0 * std::log10(down / up) > 1.0);
    // EG Time +1/3: x2 (the filter EG's attack, 0.5 s, half-way at 0.5 s instead of 0.25 s); -1/3:
    // x1/2. FEG Time moves the filter EG alone, AEG Time the amp EG alone.
    const double t0 = halfWay(sf::MD_OFF, sf::MS_FILTER_EG, 0.0f);
    const double slow = halfWay(sf::MD_EG_TIME, sf::MS_FILTER_EG, 1.0f / 3.0f);
    const double fast = halfWay(sf::MD_EG_TIME, sf::MS_FILTER_EG, -1.0f / 3.0f);
    std::printf("  EG Time: half-way at %.3f s, x2 %.3f s, x1/2 %.3f s\n", t0, slow, fast);
    CHECK(std::fabs(t0 - 0.25) < 0.03 && std::fabs(slow - 0.5) < 0.04 && std::fabs(fast - 0.125) < 0.03);
    CHECK(std::fabs(halfWay(sf::MD_FEG_TIME, sf::MS_FILTER_EG, 1.0f / 3.0f) - 0.5) < 0.04);
    CHECK(std::fabs(halfWay(sf::MD_AEG_TIME, sf::MS_FILTER_EG, 1.0f / 3.0f) - 0.25) < 0.03);
    CHECK(std::fabs(halfWay(sf::MD_AEG_TIME, sf::MS_AMP_EG, 1.0f / 3.0f) - 0.5) < 0.04);
    // Glide Time +1/3: a 0.1 s glide takes 0.2 s.
    Patch g = base();
    g.glideMode = sf::GL_ALWAYS;
    g.glideType = sf::GT_TIME;
    g.glideTime = 0.1f;
    g.mod[0].src = sf::MS_CONSTANT;
    g.mod[0].dest = sf::MD_GLIDE;
    g.mod[0].amount = 1.0f / 3.0f;
    Synth s;
    s.setPatch(g);
    s.noteOn(45, 100);
    render(s, 4410);
    s.noteOff(45);
    s.noteOn(57, 100);
    render(s, 4410);   // 0.1 s: half-way
    const float mid = s.info().pitch1;
    render(s, 4410);
    std::printf("  Glide Time x2: %.2f st after 0.1 s, %.2f after 0.2 s\n", mid, s.info().pitch1);
    CHECK(std::fabs(mid - 51.0f) < 0.3f && std::fabs(s.info().pitch1 - 57.0f) < 0.05f);
}

// The level of frequency f in x[from, from + n) (a Hann-windowed DFT bin), dB re a full-scale sine.
double toneDb(const std::vector<float>& x, double f, size_t from, size_t n) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1));
        const double ph = 2.0 * M_PI * f * static_cast<double>(i) / 44100.0;
        re += w * x[from + i] * std::cos(ph);
        im -= w * x[from + i] * std::sin(ph);
        wsum += w;
    }
    return 20.0 * std::log10(2.0 * std::hypot(re, im) / wsum + 1e-12);
}

void testHiRange() {
    std::printf("== mod busses: Hi range (audio rate)\n");
    // A triangle at 440 Hz, a sine bus at 10 Hz in Hi range (100 Hz) on 1 st of pitch: FM, its first
    // sidebands at 340 and 540 Hz (deviation ~25 Hz, index 0.25: J1 / J0 = -18 dB).
    Patch p = base();
    p.osc[0].wave = 0.0f;
    p.mod[0].src = sf::MS_SINE;
    p.mod[0].hi = true;
    p.mod[0].rateHz = 10.0f;
    p.mod[0].pitch = kOneSemi;
    p.mod[0].retrig = true;
    constexpr size_t kN = 32768;
    auto spectrum = [&](const Patch& q, std::vector<float>& x) {
        Synth s;
        s.setPatch(q);
        s.noteOn(69, 100);
        x = render(s, 4410 + static_cast<int>(kN));
        return toneDb(x, 440.0, 4410, kN);
    };
    std::vector<float> x;
    double c = spectrum(p, x);
    const double up = toneDb(x, 540.0, 4410, kN) - c, dn = toneDb(x, 340.0, 4410, kN) - c;
    std::printf("  FM at 100 Hz: sidebands %.1f / %.1f dB\n", up, dn);
    CHECK(std::fabs(up + 18.0) < 1.5 && std::fabs(dn + 18.0) < 1.5);
    Patch lowRange = p;   // the same rate without Hi: 10 Hz, no 540 Hz
    lowRange.mod[0].hi = false;
    c = spectrum(lowRange, x);
    CHECK(toneDb(x, 540.0, 4410, kN) - c < -60.0);

    // Volume at 1 kHz (rate 100, Hi), half depth: AM sidebands at 1440 and 560 Hz, 12 dB under
    // the carrier; worked out every sample, so no image of the 5.5 kHz control rate (it would put
    // sidebands around 4.5 kHz).
    Patch a = base();
    a.osc[0].wave = 0.0f;
    a.mod[0].src = sf::MS_SINE;
    a.mod[0].hi = true;
    a.mod[0].rateHz = 100.0f;
    a.mod[0].dest = sf::MD_VOLUME;
    a.mod[0].amount = 0.5f;
    a.mod[0].retrig = true;
    c = spectrum(a, x);
    const double am = toneDb(x, 1440.0, 4410, kN) - c, image = toneDb(x, 4952.5, 4410, kN) - c;
    std::printf("  AM at 1 kHz: sideband %.1f dB, control-rate image %.1f dB\n", am, image);
    CHECK(std::fabs(am + 12.0) < 1.0 && image < -70.0);

    // Wave and cutoff at audio rate change the sound, and everything stays finite.
    Patch w = base();
    w.mod[0].src = sf::MS_SAMPLE_HOLD;
    w.mod[0].hi = true;
    w.mod[0].rateHz = 30.0f;
    w.mod[0].dest = sf::MD_WAVE;
    w.mod[0].amount = 0.5f;
    w.mod[0].filter = 0.5f;
    w.cutoffHz = 1500.0f;
    Synth s;
    s.setPatch(w);
    s.noteOn(45, 100);
    const auto y = render(s, 44100);
    bool finite = true;
    for (float v : y) finite = finite && std::isfinite(v) && std::fabs(v) < 2.0f;
    CHECK(finite && rms(y, 4410, 44100) > 0.02);

    // Block sizes never change the sound, the silence between two notes included: the busses, the
    // Hi-range ones and the free-running oscillators keep time on the control grid there.
    w.mod[0].src = sf::MS_NOISE;
    w.mod[0].pitch = 0.3f;
    w.mod[1].src = sf::MS_SAMPLE_HOLD;
    w.mod[1].hi = true;
    w.mod[1].rateHz = 7.0f;
    w.mod[1].filter = 0.4f;
    w.aenv.release = 0.05f;
    auto play = [](const Patch& q, int block, bool& silent) {
        Synth s;
        s.setPatch(q);
        std::vector<float> out, L(static_cast<size_t>(block)), R(L.size());
        auto run = [&](int samples) {
            for (int done = 0; done < samples; done += block) {
                const int n = std::min(block, samples - done);
                s.render(L.data(), R.data(), n);
                out.insert(out.end(), L.begin(), L.begin() + n);
            }
        };
        s.noteOn(45, 100);
        run(8192);
        s.noteOff(45);
        run(30000);   // released, then silent
        silent = s.info().silent;
        s.noteOn(52, 100);
        run(8192);
        return out;
    };
    auto sameSound = [&play](const Patch& q) {
        bool silent128 = false, silent37 = false;
        const auto a = play(q, 128, silent128), b = play(q, 37, silent37);
        double diff = 0.0;
        for (size_t i = 0; i < a.size(); ++i) diff = std::max(diff, std::fabs(static_cast<double>(a[i]) - b[i]));
        return silent128 && silent37 && a.size() == b.size() && diff == 0.0 && rms(a, 38192, 46384) > 0.02;
    };
    CHECK(sameSound(w));   // noise and S&H, both Hi range
    Patch t = w;
    t.mod[0].src = t.mod[1].src = sf::MS_TRIANGLE;
    CHECK(sameSound(t));   // the pitch moving all along
    t.mod[0].hi = t.mod[1].hi = false;
    CHECK(sameSound(t));   // ...at the control rate
}

void testLfoKeyTrack() {
    std::printf("== mod busses: key tracking\n");
    // 100%: a 1 Hz square at C3 is 2 Hz an octave up, 0.5 Hz an octave down.
    auto halvesAt = [](int note, double period, double& up, double& down) {
        Patch p = base();
        p.mod[0].src = sf::MS_SQUARE;
        p.mod[0].rateHz = 1.0f;
        p.mod[0].pitch = kOneSemi;
        p.mod[0].retrig = true;
        p.mod[0].keyTrack = 1.0f;
        Synth s;
        s.setPatch(p);
        s.noteOn(note, 100);
        const auto x = render(s, static_cast<int>(44100 * period) + 4410);
        const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        auto st = [&](double t0, double t1) {
            return 12.0 * std::log2(pitchHz(x, static_cast<size_t>(t0 * 44100), static_cast<size_t>(t1 * 44100)) / f0);
        };
        up = st(0.1 * period, 0.4 * period);
        down = st(0.6 * period, 0.9 * period);
    };
    double up = 0.0, down = 0.0;
    halvesAt(72, 0.5, up, down);
    CHECK(up > 0.95 && down < -0.95);
    halvesAt(48, 2.0, up, down);
    CHECK(up > 0.95 && down < -0.95);

    // Hi range at 100%: the modulator follows the key, so FM stays harmonic. Rate 26.16 (261.6 Hz
    // at C3) on a triangle at C4 (523 Hz): sidebands at the carrier's octave (1047 Hz), none at
    // 785 Hz; without key tracking it stays at 261.6 Hz, and 785 Hz is there.
    auto fm = [](float keyTrack, double& octave, double& between) {
        Patch p = base();
        p.osc[0].wave = 0.0f;
        p.mod[0].src = sf::MS_SINE;
        p.mod[0].hi = true;
        p.mod[0].rateHz = 26.163f;
        p.mod[0].pitch = std::sqrt(1.0f / 6.0f);   // 4 st
        p.mod[0].retrig = true;
        p.mod[0].keyTrack = keyTrack;
        Synth s;
        s.setPatch(p);
        s.noteOn(72, 100);
        const auto x = render(s, 4410 + 32768);
        const double c = toneDb(x, 523.25, 4410, 32768);
        octave = toneDb(x, 1046.5, 4410, 32768) - c;
        between = toneDb(x, 784.9, 4410, 32768) - c;
    };
    double oct = 0.0, mid = 0.0;
    fm(1.0f, oct, mid);
    std::printf("  Hi, key tracked: %.1f dB at the octave, %.1f dB between\n", oct, mid);
    CHECK(oct > -24.0 && oct < -12.0 && mid < -50.0);
    fm(0.0f, oct, mid);
    CHECK(mid > -20.0);
}

void testPluginSide() {
    std::printf("== mod busses: the plugin side (rate text, saved state)\n");
    Host h;
    h.set(sf::P_M1_RATE, 5.0f);
    CHECK(h.display(sf::P_M1_RATE) == "5.0 Hz");
    h.set(sf::P_M1_SYNC, sf::RM_HI);
    CHECK(h.display(sf::P_M1_RATE) == "50 Hz" && h.display(sf::P_M1_SYNC) == "Hi");
    h.set(sf::P_O2_BEAT, -1.25f);
    CHECK(h.display(sf::P_O2_BEAT) == "-1.25 Hz");
    h.set(sf::P_M2_VEL, 0.5f);
    h.set(sf::P_M2_SRC, sf::MS_KEY);
    h.set(sf::P_M2_DEST, sf::MD_GLIDE);
    h.set(sf::P_M2_CTL, sf::MC_NONE);
    const std::string st = h.chunk();
    CHECK(st.find("o2_beat=-1.25\n") != std::string::npos && st.find("m1_sync=2\n") != std::string::npos &&
          st.find("m2_vel=0.5\n") != std::string::npos);
    Host g;
    CHECK(g.load(st) == 1 && g.value(sf::P_M1_SYNC) == sf::RM_HI && g.value(sf::P_M2_SRC) == sf::MS_KEY &&
          g.value(sf::P_M2_DEST) == sf::MD_GLIDE && g.value(sf::P_M2_CTL) == sf::MC_NONE &&
          std::fabs(g.value(sf::P_O2_BEAT) + 1.25f) < 1e-4f);
    // 0.0.4's: key tracking, the envelopes' options, osc 2's keys, the bend's, gated glide.
    h.set(sf::P_M2_KBT, 1.5f);
    h.set(sf::P_AE_EXP, 1);
    h.set(sf::P_FE_LATCH, 1);
    h.set(sf::P_AE_SYNC, 1 + 9);   // 1/8
    h.set(sf::P_O2_KB, sf::O2_DRONE);
    h.set(sf::P_BEND_DEST, sf::BD_OSC2);
    h.set(sf::P_GLIDE_MODE, 4);    // Legato Gated
    CHECK(h.display(sf::P_AE_SYNC) == "1/8" && h.display(sf::P_GLIDE_MODE) == "Legato Gated" &&
          h.display(sf::P_O2_KB) == "Drone" && h.display(sf::P_M2_KBT) == "150%");
    Host k;
    CHECK(k.load(h.chunk()) == 1 && std::fabs(k.value(sf::P_M2_KBT) - 1.5f) < 1e-4f && k.value(sf::P_AE_EXP) == 1 &&
          k.value(sf::P_FE_LATCH) == 1 && k.value(sf::P_AE_SYNC) == 10 && k.value(sf::P_O2_KB) == sf::O2_DRONE &&
          k.value(sf::P_BEND_DEST) == sf::BD_OSC2 && k.value(sf::P_GLIDE_MODE) == 4);
}

} // namespace

void modTests() {
    testShapesAndRate();
    testSources();
    testControl();
    testDestinations();
    testBeat();
    testMoreSources();
    testDepthAmounts();
    testMoreDestinations();
    testHiRange();
    testLfoKeyTrack();
    testPluginSide();
}

} // namespace sft
