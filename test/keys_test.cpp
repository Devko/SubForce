// How keys become notes: priority, single and multi trigger, the sustain pedal, Duo, oscillator 2's
// keys (High, Low, Drone), the bend's oscillators, and glide (Rate, Time, Exp; Always or Legato;
// gated; which oscillators).
#include "host.h"
#include "../dsp/synth.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace sft {
namespace {

using sf::Patch;
using sf::Synth;

struct Rig {
    Synth s;
    std::vector<float> L = std::vector<float>(4096), R = std::vector<float>(4096);
    explicit Rig(const Patch& p) { s.setPatch(p); }
    void run(int samples) {
        while (samples > 0) {
            const int m = std::min(samples, 4096);
            s.render(L.data(), R.data(), m);
            samples -= m;
        }
    }
};

void testPriority() {
    std::printf("== note priority, retrigger, pedal\n");
    for (int prio : {sf::PR_LAST, sf::PR_LOW, sf::PR_HIGH}) {
        Patch p;
        p.priority = prio;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.s.noteOn(64, 100);
        r.s.noteOn(55, 100);
        r.run(64);
        const int want = prio == sf::PR_LAST ? 55 : prio == sf::PR_LOW ? 55 : 64;
        CHECK(r.s.info().note1 == want && r.s.info().note2 == want);   // Mono: both oscillators
        r.s.noteOff(55);   // back to a held key
        r.run(64);
        const int back = prio == sf::PR_LAST ? 64 : prio == sf::PR_LOW ? 60 : 64;
        CHECK(r.s.info().note1 == back && r.s.info().gate);
        r.s.noteOff(60);
        r.s.noteOff(64);
        r.run(64);
        CHECK(!r.s.info().gate && r.s.info().held == 0);
    }
    // Low priority: a higher key over a held low one changes nothing (no retrigger either).
    {
        Patch p;
        p.priority = sf::PR_LOW;
        p.aenv.attack = 0.001f;
        p.aenv.decay = 0.05f;
        p.aenv.sustain = 0.3f;
        Rig r(p);
        r.s.noteOn(40, 100);
        r.run(22050);
        const float before = r.s.info().ampEnv;
        r.s.noteOn(52, 100);
        r.run(441);
        CHECK(r.s.info().note1 == 40 && std::fabs(r.s.info().ampEnv - before) < 0.02f);
    }
    // Multi: a new key restarts the envelopes. Single: legato keys don't.
    for (int trig : {sf::TR_MULTI, sf::TR_SINGLE}) {
        Patch p;
        p.trigger = trig;
        p.aenv.attack = 0.05f;
        p.aenv.decay = 0.05f;
        p.aenv.sustain = 0.3f;
        Rig r(p);
        r.s.noteOn(48, 100);
        r.run(22050);
        CHECK(std::fabs(r.s.info().ampEnv - 0.3f) < 0.01f);
        r.s.noteOn(50, 100);
        r.run(1323);   // 30 ms into a 50 ms attack, or still on the sustain
        CHECK(trig == sf::TR_MULTI ? r.s.info().ampEnv > 0.6f : std::fabs(r.s.info().ampEnv - 0.3f) < 0.01f);
        CHECK(r.s.info().note1 == 50);
        // ...and the first key after all were up always does.
        r.s.noteOff(48);
        r.s.noteOff(50);
        r.run(44100);
        r.s.noteOn(48, 100);
        r.run(1323);
        CHECK(r.s.info().ampEnv > 0.5f);   // 30 ms into the linear 50 ms attack
        // Back to a key still held when the newer one lifts: the oscillators move, the envelopes
        // don't start again (Multi retriggers on key presses, as on the original).
        r.run(22050);
        r.s.noteOn(55, 100);
        r.run(22050);
        r.s.noteOff(55);
        r.run(1323);
        CHECK(r.s.info().note1 == 48 && std::fabs(r.s.info().ampEnv - 0.3f) < 0.01f);
    }
    // Duo: one key of a pair lifting doesn't re-attack the other.
    {
        Patch p;
        p.keyMode = sf::KM_DUO;
        p.aenv.attack = 0.05f;
        p.aenv.decay = 0.05f;
        p.aenv.sustain = 0.3f;
        Rig r(p);
        r.s.noteOn(48, 100);
        r.s.noteOn(55, 100);
        r.run(22050);
        r.s.noteOff(55);
        r.run(1323);
        CHECK(r.s.info().note2 == 48 && std::fabs(r.s.info().ampEnv - 0.3f) < 0.01f);
    }
    // The pedal holds the last note until it lifts.
    {
        Patch p;
        Rig r(p);
        r.s.noteOn(48, 100);
        r.s.sustain(true);
        r.s.noteOff(48);
        r.run(22050);
        CHECK(r.s.info().gate && r.s.info().ampEnv > 0.3f);
        r.s.sustain(false);
        r.run(22050);
        CHECK(!r.s.info().gate);
    }
    // A key repeated, more than 16 keys, a note-off for a key never pressed.
    {
        Patch p;
        Rig r(p);
        for (int i = 0; i < 24; ++i) r.s.noteOn(40 + i, 100);
        r.s.noteOn(41, 100);
        CHECK(r.s.info().held == Synth::kHeldMax && r.s.info().note1 == 41);
        r.s.noteOff(100);
        CHECK(r.s.info().held == Synth::kHeldMax);
        for (int i = 0; i < 24; ++i) r.s.noteOff(40 + i);
        r.run(64);
        CHECK(r.s.info().held == 0 && !r.s.info().gate);
    }
}

void testRestrike() {
    std::printf("== the same key again, catching up, mode changes\n");
    // A sounding key struck again: Multi starts a new attack, Single doesn't.
    for (int trig : {sf::TR_MULTI, sf::TR_SINGLE}) {
        Patch p;
        p.trigger = trig;
        p.aenv.attack = 0.05f;
        p.aenv.decay = 0.05f;
        p.aenv.sustain = 0.3f;
        Rig r(p);
        r.s.noteOn(48, 100);
        r.s.sustain(true);
        r.s.noteOff(48);   // held by the pedal
        r.run(22050);
        r.s.noteOn(48, 100);
        r.run(1323);
        CHECK(trig == sf::TR_MULTI ? r.s.info().ampEnv > 0.6f : std::fabs(r.s.info().ampEnv - 0.3f) < 0.01f);
        Rig d(p);   // a repeated note-on with no note-off between (a sequencer's overlap)
        d.s.noteOn(50, 100);
        d.run(22050);
        d.s.noteOn(50, 100);
        d.run(1323);
        CHECK(trig == sf::TR_MULTI ? d.s.info().ampEnv > 0.6f : std::fabs(d.s.info().ampEnv - 0.3f) < 0.01f);
    }
    // A note between control steps glides from that sample: a 2 ms (88-sample) glide is in the
    // same place 80 samples on, wherever in a control step the note came (the pitch reads as of
    // the last control step: 72 samples in), and has arrived after 96.
    float at80[3] = {};
    for (int k = 0; k < 3; ++k) {
        Patch p;
        p.glideMode = sf::GL_ALWAYS;
        p.glideTime = 88.0f / 44100.0f;
        Rig r(p);
        r.s.noteOn(40, 100);
        r.run(64 + 3 * k);
        r.s.noteOn(52, 100);
        r.run(80);
        at80[k] = r.s.info().pitch1;
        r.run(16);
        CHECK(r.s.info().pitch1 == 52.0f);
    }
    CHECK(std::fabs(at80[0] - (52.0f - 12.0f * 16.0f / 88.0f)) < 1e-3f && at80[1] == at80[0] && at80[2] == at80[0]);
    // Mono <-> Duo with keys down: the oscillators take the new rule's keys at once.
    Patch p;
    p.keyMode = sf::KM_DUO;
    Rig r(p);
    r.s.noteOn(48, 100);
    r.s.noteOn(55, 100);
    r.run(64);
    CHECK(r.s.info().note1 == 55 && r.s.info().note2 == 48);
    p.keyMode = sf::KM_MONO;
    r.s.setPatch(p);
    r.run(64);
    CHECK(r.s.info().note1 == 55 && r.s.info().note2 == 55 && r.s.info().gate);
    p.priority = sf::PR_LOW;
    r.s.setPatch(p);
    r.run(64);
    CHECK(r.s.info().note1 == 48 && r.s.info().note2 == 48);
}

void testDuo() {
    std::printf("== Duo\n");
    Patch p;
    p.keyMode = sf::KM_DUO;
    p.priority = sf::PR_LOW;
    Rig r(p);
    r.s.noteOn(48, 100);
    r.run(64);
    CHECK(r.s.info().note1 == 48 && r.s.info().note2 == 48 && r.s.activeVoices() == 1);
    r.s.noteOn(55, 100);
    r.run(64);
    CHECK(r.s.info().note1 == 48 && r.s.info().note2 == 55 && r.s.activeVoices() == 2);
    r.s.noteOn(52, 100);   // Low: osc 1 the lowest, osc 2 the next
    r.run(64);
    CHECK(r.s.info().note1 == 48 && r.s.info().note2 == 52);
    r.s.noteOff(48);
    r.run(64);
    CHECK(r.s.info().note1 == 52 && r.s.info().note2 == 55);
    // Last: the two newest.
    Patch q = p;
    q.priority = sf::PR_LAST;
    Rig l(q);
    l.s.noteOn(60, 100);
    l.s.noteOn(67, 100);
    l.s.noteOn(64, 100);
    l.run(64);
    CHECK(l.s.info().note1 == 64 && l.s.info().note2 == 67);

    // Each oscillator really plays its key: osc 2 alone sounds the second note.
    Host h;
    h.bare();
    h.set(sf::P_KMODE, sf::KM_DUO);
    h.set(sf::P_PRIO, sf::PR_LOW);
    h.set(sf::P_MIX_O1, 0.0f);
    h.set(sf::P_MIX_O2, 0.8f);
    h.on(45);
    h.on(57);
    h.run(kBlocksPerSec / 4);
    h.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(h.L) - 220.0) < 0.5);
}

void testGlide() {
    std::printf("== glide\n");
    // Time: every interval takes the glide time. Rate: the time per octave.
    for (int type : {sf::GT_TIME, sf::GT_RATE, sf::GT_EXP})
        for (int interval : {12, 24}) {
            Patch p;
            p.glideMode = sf::GL_ALWAYS;
            p.glideType = type;
            p.glideTime = 0.2f;
            Rig r(p);
            r.s.noteOn(36, 100);
            r.run(4410);
            r.s.noteOn(36 + interval, 100);
            float mid = 0.0f;
            int t = 0;
            while (std::fabs(r.s.info().pitch1 - (36.0f + interval)) > 0.12f * interval && t < 441000) {
                r.run(32);
                t += 32;
                if (t == 4416) mid = r.s.info().pitch1 - 36.0f;   // ~100 ms
            }
            if (t < 4416) {
                r.run(4416 - t);
                mid = r.s.info().pitch1 - 36.0f;
            }
            const double ms = t / 44.1;
            std::printf("  %s %2d st: half-way %.1f st, within 12%% after %.0f ms\n",
                        type == sf::GT_TIME ? "Time" : type == sf::GT_RATE ? "Rate" : "Exp ", interval, mid, ms);
            if (type == sf::GT_TIME) CHECK(std::fabs(mid - 0.5f * interval) < 0.6f && ms > 170 && ms < 185);
            if (type == sf::GT_RATE) CHECK(std::fabs(mid - 6.0f) < 0.6f && std::fabs(ms - 176.0 * interval / 12.0) < 10.0);
            if (type == sf::GT_EXP) CHECK(mid > 0.85f * interval && ms > 80 && ms < 105);   // 99% at the glide time: 12% at 2.1 RC
        }
    // Legato glides only between overlapping keys; Always also from the last note.
    for (int mode : {sf::GL_LEGATO, sf::GL_ALWAYS}) {
        Patch p;
        p.glideMode = mode;
        p.glideTime = 0.2f;
        Rig r(p);
        r.s.noteOn(40, 100);
        r.run(441);
        r.s.noteOff(40);
        r.run(441);
        r.s.noteOn(52, 100);   // detached
        r.run(441);
        CHECK(mode == sf::GL_ALWAYS ? r.s.info().pitch1 < 50.0f : r.s.info().pitch1 == 52.0f);
        r.s.noteOn(64, 100);   // overlapping
        r.run(441);
        CHECK(r.s.info().pitch1 < 62.0f);
    }
    // Off: jumps. Dest Osc 2: osc 1 jumps, osc 2 glides.
    {
        Patch p;
        p.glideMode = sf::GL_OFF;
        Rig r(p);
        r.s.noteOn(40, 100);
        r.run(64);
        r.s.noteOn(52, 100);
        r.run(8);
        CHECK(r.s.info().pitch1 == 52.0f);
        Patch q;
        q.glideMode = sf::GL_ALWAYS;
        q.glideDest = sf::OD_OSC2;
        q.glideTime = 0.2f;
        Rig d(q);
        d.s.noteOn(40, 100);
        d.run(64);
        d.s.noteOn(52, 100);
        d.run(441);
        CHECK(d.s.info().pitch1 == 52.0f && d.s.info().pitch2 < 50.0f);
    }
    // The sounding pitch glides too (not just the bookkeeping): a 1-octave Time glide heard.
    Host h;
    h.bare();
    h.set(sf::P_GLIDE_MODE, sf::GL_ALWAYS);
    h.set(sf::P_GLIDE, 0.3f);
    h.on(45);
    h.run(kBlocksPerSec / 2);
    h.on(57);
    h.run(kBlocksPerSec / 10);   // the first 100 ms of a 300 ms glide
    const double during = pitchHz(h.L);
    h.run(kBlocksPerSec);
    CHECK(during > 115.0 && during < 190.0 && std::fabs(pitchHz(h.L, h.L.size() / 2) - 220.0) < 0.5);
}

// Oscillator 2's keys (the original's KB CTRL), the pitch bend's oscillators, gated glide.
void testOsc2KeysBendGate() {
    std::printf("== osc 2's keys, the bend's oscillators, gated glide\n");
    // Duo High: osc 2 on the highest key, osc 1 on the lowest, whatever the order and priority;
    // Low the other way round.
    for (int mode : {sf::O2_HIGH, sf::O2_LOW}) {
        Patch p;
        p.keyMode = sf::KM_DUO;
        p.priority = sf::PR_LAST;
        p.osc2Keys = mode;
        Rig r(p);
        for (int n : {60, 48, 72, 55}) r.s.noteOn(n, 100);
        r.run(64);
        const bool high = mode == sf::O2_HIGH;
        CHECK(r.s.info().note1 == (high ? 48 : 72) && r.s.info().note2 == (high ? 72 : 48));
        r.s.noteOff(72);
        r.run(64);
        CHECK(r.s.info().note1 == (high ? 48 : 60) && r.s.info().note2 == (high ? 60 : 48));
    }
    // Mono: High changes nothing (both oscillators on the key by priority).
    Patch m;
    m.osc2Keys = sf::O2_HIGH;
    Rig mono(m);
    mono.s.noteOn(60, 100);
    mono.s.noteOn(48, 100);
    mono.run(64);
    CHECK(mono.s.info().note1 == 48 && mono.s.info().note2 == 48);

    // The pitch of one oscillator alone, `note` played, the bend up (2 st) or not.
    auto pitchOf = [](Patch p, int osc, int note, bool bend) {
        p.cutoffHz = 20000.0f;
        p.drift = 0.0f;
        p.envAmount = 0.0f;
        p.mixOsc1 = osc == 1 ? 0.8f : 0.0f;
        p.mixOsc2 = osc == 2 ? 0.8f : 0.0f;
        Synth s;
        s.setPatch(p);
        s.noteOn(note, 100);
        if (bend) s.pitchBend(1.0f);
        std::vector<float> L(22050), R(L.size());
        for (size_t b = 0; b < L.size(); b += 128) s.render(&L[b], &R[b], static_cast<int>(std::min<size_t>(128, L.size() - b)));
        return pitchHz(L, 4410, L.size());
    };
    // Drone: osc 2 at C3 whatever the key; its frequency knob reaches +-3 octaves.
    Patch d;
    d.osc2Keys = sf::O2_DRONE;
    CHECK(std::fabs(pitchOf(d, 2, 45, false) - 261.63) < 0.3 && std::fabs(pitchOf(d, 2, 81, false) - 261.63) < 0.3);
    CHECK(std::fabs(pitchOf(d, 1, 45, false) - 110.0) < 0.2);   // osc 1 still plays the key
    d.osc2Semis = 7.0f;   // +36 st: C6
    CHECK(std::fabs(pitchOf(d, 2, 45, false) - 2093.0) < 3.0);
    d.osc2Semis = -3.5f;  // -18 st
    CHECK(std::fabs(pitchOf(d, 2, 45, false) - 92.5) < 0.2);

    // Bend To: which oscillators the bend moves (2 st up: 220 -> 246.9 Hz).
    const double up = 246.94, still = 220.0;
    for (int dest : {sf::BD_BOTH, sf::BD_OSC1, sf::BD_OSC2, sf::BD_OFF}) {
        Patch b;
        b.bendDest = dest;
        const double o1 = pitchOf(b, 1, 57, true), o2 = pitchOf(b, 2, 57, true);
        const bool b1 = dest == sf::BD_BOTH || dest == sf::BD_OSC1, b2 = dest == sf::BD_BOTH || dest == sf::BD_OSC2;
        CHECK(std::fabs(o1 - (b1 ? up : still)) < 0.3 && std::fabs(o2 - (b2 ? up : still)) < 0.3);
    }

    // Gated glide: a 1 s glide stops where it is when the key goes up; not gated, it goes on.
    for (bool gated : {true, false}) {
        Patch g;
        g.glideMode = sf::GL_ALWAYS;
        g.glideType = sf::GT_TIME;
        g.glideTime = 1.0f;
        g.glideGated = gated;
        Rig r(g);
        r.s.noteOn(48, 100);
        r.run(441);
        r.s.noteOff(48);
        r.s.noteOn(60, 100);
        r.run(11025);   // a quarter of the way
        r.s.noteOff(60);
        const float at = r.s.info().pitch1;
        r.run(11025);
        const float later = r.s.info().pitch1;
        std::printf("  %s glide: %.2f st at the release, %.2f a quarter second later\n", gated ? "gated" : "free", at, later);
        if (gated) CHECK(std::fabs(at - 51.0f) < 0.1f && std::fabs(later - at) < 1e-4f);
        else CHECK(std::fabs(later - 54.0f) < 0.1f);
    }
}

} // namespace

void keyTests() {
    testPriority();
    testRestrike();
    testDuo();
    testGlide();
    testOsc2KeysBendGate();
}

} // namespace sft
