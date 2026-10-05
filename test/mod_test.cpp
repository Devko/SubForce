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

void testDestinations() {
    std::printf("== mod busses: destinations\n");
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
}

} // namespace

void modTests() {
    testShapesAndRate();
    testControl();
    testDestinations();
}

} // namespace sft
