// How keys become notes: priority, single and multi trigger, the sustain pedal, Duo, and glide
// (Rate, Time, Exp; Always or Legato; which oscillators).
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
        CHECK(r.s.info().ampEnv > 0.6f);
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
            r.run(4410);   // 100 ms
            const float mid = r.s.info().pitch1 - 36.0f;
            int t = 4410;
            while (std::fabs(r.s.info().pitch1 - (36.0f + interval)) > 0.12f * interval && t < 441000) {
                r.run(32);
                t += 32;
            }
            const double ms = t / 44.1;
            std::printf("  %s %2d st: half-way %.1f st, within 12%% after %.0f ms\n",
                        type == sf::GT_TIME ? "Time" : type == sf::GT_RATE ? "Rate" : "Exp ", interval, mid, ms);
            if (type == sf::GT_TIME) CHECK(std::fabs(mid - 0.5f * interval) < 0.6f && ms > 170 && ms < 185);
            if (type == sf::GT_RATE) CHECK(std::fabs(mid - 6.0f) < 0.6f && std::fabs(ms - 176.0 * interval / 12.0) < 10.0);
            if (type == sf::GT_EXP) CHECK(mid > 0.6f * interval && ms < 110);   // fast at first, the tail long
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

} // namespace

void keyTests() {
    testPriority();
    testDuo();
    testGlide();
}

} // namespace sft
