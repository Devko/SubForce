// The test suite: the engine's DSP measured directly (engine_test.cpp), then the whole plugin
// driven through its VST2 entry points the way MPC drives it (128-frame blocks, events with
// deltaFrames, 0..1 params). Built with ASan/UBSan by `make test`, for the Force's CPU under
// qemu by `make test-arm`.
#include "host.h"
#include "../plugin/surface.h"

#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace sft {
int g_fail = 0, g_pass = 0;

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == 1) return 2400;   // audioMasterVersion
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&log->time);
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/sftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/sftest");
    }();
    return dir;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
    return to > from ? std::sqrt(s / static_cast<double>(to - from)) : 0.0;
}

double pitchHz(const std::vector<float>& x, size_t from, size_t to) {
    if (to == 0 || to > x.size()) to = x.size();
    double first = -1.0, last = 0.0;
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++n;
            }
        }
    return n > 0 ? 44100.0 * n / (last - first) : 0.0;
}

namespace {
using cd = std::complex<double>;
void fft(std::vector<cd>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const cd u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
} // namespace

// The strongest spectral peak below 20 kHz that is not a harmonic of f0, against the strongest
// peak of all, in dB. A 4-term Blackman-Harris window (sidelobes under -92 dB); x.size() must
// be a power of two.
double worstAliasDb(const std::vector<float>& x, double f0) {
    const size_t n = x.size();
    std::vector<cd> a(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1);
        a[i] = x[i] * (0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
    }
    fft(a);
    const double bin = 44100.0 / static_cast<double>(n);
    double strongest = 0.0, worst = 1e-30;
    for (size_t k = 4; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * bin, m = std::abs(a[k]);
        if (f > 20000.0) break;
        strongest = std::max(strongest, m);
        const double h = f / f0;
        if (std::fabs(h - std::round(h)) * f0 < 6.0 * bin) continue;
        worst = std::max(worst, m);
    }
    return 20.0 * std::log10(worst / strongest);
}

} // namespace sft

namespace {
using namespace sft;

void testBasics() {
    std::printf("== plugin basics\n");
    Host h;
    CHECK(h.e && h.e->magic == vst::kMagic);
    CHECK(h.e->numParams == sf::P_COUNT && h.e->numOutputs == 2 && h.e->numInputs == 0);
    CHECK((h.e->flags & vst::effFlagsIsSynth) && (h.e->flags & vst::effFlagsProgramChunks));
    char b[256] = {};
    h.e->dispatcher(h.e, vst::effGetEffectName, 0, 0, b, 0.0f);
    CHECK(std::string(b) == "SubForce");
    CHECK(h.e->dispatcher(h.e, vst::effCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent"), 0.0f) == 1);
    CHECK(h.e->dispatcher(h.e, vst::effCanDo, 0, 0, const_cast<char*>("sendVstMidiEvent"), 0.0f) == -1);
    CHECK(h.e->dispatcher(h.e, vst::effGetPlugCategory, 0, 0, nullptr, 0.0f) == vst::kPlugCategSynth);
    // Every parameter has a short, unique name; every sound parameter is automatable.
    bool namesOk = true, autoOk = true;
    std::map<std::string, int> seen;
    for (int i = 0; i < sf::P_COUNT; ++i) {
        const std::string n = h.name(i);
        namesOk = namesOk && !n.empty() && n.size() <= 24 && seen.count(n) == 0;
        seen[n] = i;
        const bool automatable = h.e->dispatcher(h.e, vst::effCanBeAutomated, i, 0, nullptr, 0.0f) == 1;
        autoOk = autoOk && automatable == (sf::PARAM_INFO[i].kind == sf::Kind::Synth);
    }
    CHECK(namesOk && autoOk);
    CHECK(h.display(sf::P_F_CUT) == "2.00 kHz" && h.display(sf::P_F_SLOPE) == "24 dB");
    CHECK(h.display(sf::P_O1_WAVE) == "Saw" && h.display(sf::P_O1_OCT) == "8'");
    CHECK(h.display(sf::P_O2_FREQ) == "0.00 st" && h.display(sf::P_NOISE_COLOR) == "Pink");
    h.set(sf::P_O1_WAVE, 1.0f);
    CHECK(h.display(sf::P_O1_WAVE) == "Pulse 6%");
    h.set(sf::P_O1_WAVE, 0.5f);
    CHECK(h.display(sf::P_O1_WAVE) == "Saw-Sqr 50%");
    h.set(sf::P_O1_WAVE, 0.0f);
    CHECK(h.display(sf::P_O1_WAVE) == "Triangle");
    h.set(sf::P_F_ENV, 1.0f);
    CHECK(h.display(sf::P_F_ENV) == "+10.00 oct");
    h.set(sf::P_M1_PITCH, -0.5f);
    CHECK(h.display(sf::P_M1_PITCH) == "-6.0 st");
    h.set(sf::P_VOLUME, -60.0f);
    CHECK(h.display(sf::P_VOLUME) == "-inf dB");
    // Out-of-range indices and NaN from the host are harmless.
    h.e->setParameter(h.e, -1, 0.5f);
    h.e->setParameter(h.e, sf::P_COUNT, 0.5f);
    CHECK(h.e->getParameter(h.e, sf::P_COUNT) == 0.0f);
    h.e->setParameter(h.e, sf::P_F_CUT, std::nanf(""));
    CHECK(h.get(sf::P_F_CUT) == 0.0f);
    h.e->setParameter(h.e, sf::P_STATUS, 0.7f);   // the status readout ignores writes
    CHECK(h.get(sf::P_STATUS) == 0.0f);
}

void testPlay() {
    std::printf("== notes in, sound out\n");
    Host h;
    CHECK(h.run(8) == 0.0f);   // nothing before a note
    h.on(48, 100, 64);         // starts at sample 64 of the next block
    h.run(1);
    float before = 0.0f;
    for (int i = 0; i < 64; ++i) before = std::max(before, std::fabs(h.L[static_cast<size_t>(i)]));
    CHECK(before == 0.0f);
    const float peak = h.run(kBlocksPerSec);
    CHECK(h.finite && peak > 0.05f && peak < 1.0f);
    bool mono = true;
    for (size_t i = 0; i < h.L.size(); ++i) mono = mono && h.L[i] == h.R[i];
    CHECK(mono);
    CHECK(h.voices() == 1);
    CHECK(h.display(sf::P_STATUS).compare(0, 9, "VOICES 1 ") == 0);
    h.off(48);
    h.run(kBlocksPerSec);   // the default release is 150 ms
    CHECK(h.run(4) == 0.0f);
    CHECK(h.voices() == 0);

    // Pitch: A3 is 220 Hz (the saw through an open filter).
    Host p;
    p.bare();
    p.on(57, 100);
    p.run(kBlocksPerSec / 4);
    p.run(kBlocksPerSec);
    CHECK(std::fabs(pitchHz(p.L) - 220.0) < 0.3);
    // Octave switches, osc 2's frequency, pitch bend.
    p.set(sf::P_O1_OCT, 3);   // 4'
    p.run(8);
    p.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(p.L) - 440.0) < 0.6);
    p.set(sf::P_O1_OCT, 2);
    p.set(sf::P_MIX_O1, 0.0f);
    p.set(sf::P_MIX_O2, 0.8f);
    p.set(sf::P_O2_FREQ, 7.0f);
    p.run(8);
    p.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(p.L) - 220.0 * std::pow(2.0, 7.0 / 12.0)) < 0.6);
    p.set(sf::P_O2_FREQ, 0.0f);
    p.set(sf::P_BEND_UP, 12);
    p.midi(0xE0, 0x7F, 0x7F);   // full bend up
    p.run(8);
    p.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(p.L) - 440.0) < 1.0);
    p.midi(0xE0, 0x00, 0x40);   // centre
    p.run(8);
    p.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(p.L) - 220.0) < 0.5);

    // All sound off (CC 120) is immediate; all notes off (CC 123) releases.
    p.midi(0xB0, 120, 0);
    CHECK(p.run(1) == 0.0f);
    p.on(57);
    p.run(10);
    p.midi(0xB0, 123, 0);
    p.run(kBlocksPerSec);
    CHECK(p.run(2) == 0.0f);
    // Suspend (MPC stopping the track) silences too.
    p.on(57);
    p.run(10);
    p.e->dispatcher(p.e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
    CHECK(p.run(1) == 0.0f);
    CHECK(p.finite);
}

void testProcessLegacy() {
    std::printf("== process() (accumulating)\n");
    Host a, b;
    a.on(45, 110, 300);   // one 1024-frame call (split into 512-frame sub-blocks inside)
    std::vector<float> L(1024, 0.25f), R(1024, 0.25f);   // process() adds to what is there
    float* out[2] = {L.data(), R.data()};
    a.e->process(a.e, nullptr, out, 1024);
    // The same note at the same sample through processReplacing in 128-frame blocks: sample 300 is
    // in the third block, 44 frames in.
    std::vector<float> L2(1024), R2(1024);
    for (int k = 0; k < 8; ++k) {
        if (k == 2) b.on(45, 110, 300 - 256);
        float* o[2] = {L2.data() + k * 128, R2.data() + k * 128};
        b.e->processReplacing(b.e, nullptr, o, 128);
    }
    double before = 0.0, diff = 0.0, energy = 0.0;
    for (int i = 0; i < 300; ++i) before = std::max(before, std::fabs(static_cast<double>(L[i]) - 0.25));
    for (int i = 0; i < 1024; ++i) {
        diff = std::max(diff, std::fabs(static_cast<double>(L[i]) - 0.25 - L2[i]));
        energy += std::fabs(L2[i]);
    }
    CHECK(before == 0.0 && energy > 1.0);
    CHECK(diff < 1e-6);   // block sizes don't change the sound
}

void testMidiMapping() {
    std::printf("== MIDI: pedal, mod wheel, pressure, bend down\n");
    Host h;
    h.bare();
    h.set(sf::P_AE_R, 0.01f);
    // CC 64 holds the note after its key goes up.
    h.on(57);
    h.midi(0xB0, 64, 127);
    h.off(57);
    h.run(kBlocksPerSec / 2);
    CHECK(rms(h.L) > 0.05);
    h.midi(0xB0, 64, 0);
    h.run(kBlocksPerSec / 4);
    CHECK(h.run(2) == 0.0f);
    // CC 1, channel pressure and the sounding key's own pressure (poly aftertouch: what pads
    // send) reach a bus set to them: a square on pitch, one semitone. Another key's doesn't.
    for (int how = 0; how < 4; ++how) {
        const int ctl = how == 0 ? sf::MC_MODWHEEL : sf::MC_AFTERTOUCH;
        Host m;
        m.bare();
        m.set(sf::P_M1_SRC, sf::MS_SQUARE);
        m.set(sf::P_M1_RATE, 0.25f);   // 2 s up, 2 s down
        m.set(sf::P_M1_PITCH, std::sqrt(1.0f / 24.0f));
        m.set(sf::P_M1_CTL, ctl);
        m.set(sf::P_M1_TRIG, 1);
        m.on(57);
        m.run(kBlocksPerSec / 2);
        const double off = pitchHz(m.L);
        if (how == 0) m.midi(0xB0, 1, 127);
        else if (how == 1) m.midi(0xD0, 127, 0);
        else m.midi(0xA0, how == 2 ? 57 : 60, 127);
        m.run(4);
        m.run(kBlocksPerSec / 2);
        const double up = how == 3 ? 1.0 : std::pow(2.0, 1.0 / 12.0);
        CHECK(std::fabs(off - 220.0) < 0.5 && std::fabs(pitchHz(m.L) / 220.0 - up) < 0.003);
    }
    // Bend down by its own range.
    h.set(sf::P_BEND_DN, 12);
    CHECK(h.display(sf::P_BEND_DN) == "12 st");
    h.on(57);
    h.midi(0xE0, 0x00, 0x00);   // full bend down
    h.run(8);
    h.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(h.L) - 110.0) < 0.4);
    // CC 121 (reset all controllers): the bend is back at rest.
    h.midi(0xB0, 121, 0);
    h.run(8);
    h.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(h.L) - 220.0) < 0.5);
}

void testStress() {
    std::printf("== stress: floods, extremes, random values\n");
    Host h;
    // More events than the queue holds: the note-offs still get through.
    for (int r = 0; r < 4; ++r) {
        for (int i = 0; i < 600; ++i) h.on(36 + i % 60, 1 + i % 127, i % 128);
        for (int i = 0; i < 60; ++i) h.off(36 + i, kBlock - 1);   // after every note-on of the block
        h.run(1);
    }
    h.run(kBlocksPerSec * 2);
    CHECK(h.run(2) == 0.0f);
    // Every sound parameter at random values, notes going: finite and bounded.
    uint32_t s = 12345;
    auto rnd = [&s] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) / 16777216.0f;
    };
    float worst = 0.0f;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < sf::P_COUNT; ++i)
            if (sf::PARAM_INFO[i].kind == sf::Kind::Synth && i != sf::P_VOLUME) h.setN(i, rnd());
        h.set(sf::P_VOLUME, 0.0f);
        h.on(24 + static_cast<int>(rnd() * 84), 1 + static_cast<int>(rnd() * 126));
        h.midi(0xE0, static_cast<uint8_t>(rnd() * 127), static_cast<uint8_t>(rnd() * 127));
        h.midi(0xB0, 1, static_cast<uint8_t>(rnd() * 127));
        h.midi(0xD0, static_cast<uint8_t>(rnd() * 127), 0);
        worst = std::max(worst, h.run(20));
        if (rnd() < 0.5f) h.midi(0xB0, 123, 0);
    }
    CHECK(h.finite);
    std::printf("  peak over 40 random patches: %.2f\n", worst);
    CHECK(worst < 2.0f);
}

} // namespace

long long sft::g_msPerEvent = 1000;

int main() {
    using namespace sft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    setenv("SF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("SF_DATA_DIR", (root + "/data").c_str(), 1);
    setenv("SF_FIXED_SEED", "1", 1);   // every instance the same random numbers: they are compared
    // Host events a second apart unless a test says otherwise (sft::Turn): stepping never
    // mistakes two of them for one turn, whatever the machine's speed (and qemu's).
    sf::Surface::clock = [] {
        static long long t = 0;
        return t += g_msPerEvent;
    };

    engineTests();
    testBasics();
    testPlay();
    testProcessLegacy();
    testMidiMapping();
    keyTests();
    modTests();
    presetTests();
    testStress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
