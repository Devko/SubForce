// CPU bench for the built plugin: dlopen()s the .so like MPC does, plays notes at 44.1 kHz /
// 128-frame blocks and times every processReplacing call with the thread's own CPU clock. Same
// verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   sfbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases: idle (no note), the Init patch on a held note, a heavy patch (both oscillators, sub,
// noise, feedback, sync, full Multidrive and high resonance; bus 1 a 60 Hz triangle on pitch,
// cutoff and Multidrive, bus 2 a 7 Hz smooth random on pitch, cutoff and both waves; a looping
// filter envelope, Duo on two keys), the heavy patch retriggered every 50 ms with glide, and the
// heavy patch with both busses in Hi range, or with an LFO on EG Time and a Hi-range bus.
// Hermetic: the plugin reads no user folders and saves nothing.
// A profiling build of the plugin (make arm-bench-stages: subforce_stages.so) also reports where
// each case's time goes.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../dsp/synth.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kBudgetUs = kBlock * 1e6 / 44100.0;   // 2902 us

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

float norm(int id, float v) {
    const sf::ParamSpec& s = sf::PARAM_SPECS[id];
    float n = 0.0f;
    if (s.curve == sf::Curve::Log) n = std::log(v / s.lo) / std::log(s.hi / s.lo);
    else if (s.curve == sf::Curve::Pow) n = s.hi > 0.0f ? std::cbrt(v / s.hi) : 0.0f;
    else n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f;
    return std::clamp(n, 0.0f, 1.0f);
}

void midi(AEffect* e, uint8_t st, uint8_t d1, uint8_t d2) {
    VstMidiEvent ev{};
    ev.type = vst::kVstMidiType;
    ev.byteSize = sizeof ev;
    ev.midiData[0] = st;
    ev.midiData[1] = d1;
    ev.midiData[2] = d2;
    VstEvents evs{};
    evs.numEvents = 1;
    evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
    e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
}

void heavy(AEffect* e) {
    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
    set(sf::P_MIX_O1, 0.9f);
    set(sf::P_MIX_O2, 0.8f);
    set(sf::P_MIX_SUB, 0.6f);
    set(sf::P_MIX_NOISE, 0.3f);
    set(sf::P_MIX_FB, 0.5f);
    set(sf::P_O2_SYNC, 1);
    set(sf::P_O2_FREQ, 3.7f);
    set(sf::P_F_DRIVE, 1.0f);
    set(sf::P_F_RES, 0.85f);
    set(sf::P_F_ENV, 0.6f);
    set(sf::P_FE_LOOP, 1);
    set(sf::P_FE_D, 0.08f);
    set(sf::P_KMODE, sf::KM_DUO);
    set(sf::P_DRIFT, 1.0f);
    for (int b = 0; b < 2; ++b) {
        const int d = b * (sf::P_M2_SRC - sf::P_M1_SRC);
        set(sf::P_M1_SRC + d, b ? sf::MS_SMOOTH : sf::MS_TRIANGLE);
        set(sf::P_M1_RATE + d, b ? 7.0f : 60.0f);
        set(sf::P_M1_PITCH + d, 0.3f);
        set(sf::P_M1_FILTER + d, 0.6f);
        set(sf::P_M1_DEST + d, b ? sf::MD_WAVE : sf::MD_DRIVE);
        set(sf::P_M1_AMT + d, 0.5f);
        set(sf::P_M1_CTL + d, sf::MC_ALWAYS);
    }
}

// The busses at their dearest: both in Hi range (worked out every sample) on pitch, cutoff,
// wave and volume; or an LFO on EG Time (new envelope coefficients every control step) and a
// Hi-range FM bus.
void hiRange(AEffect* e, bool egTime) {
    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
    set(sf::P_O2_BEAT, 1.2f);
    for (int b = 0; b < 2; ++b) {
        const int d = b * (sf::P_M2_SRC - sf::P_M1_SRC);
        const bool eg = egTime && b == 0;
        set(sf::P_M1_SRC + d, b ? sf::MS_NOISE : sf::MS_SINE);
        set(sf::P_M1_SYNC + d, eg ? sf::RM_FREE : sf::RM_HI);
        set(sf::P_M1_RATE + d, eg ? 3.0f : b ? 50.0f : 22.0f);
        set(sf::P_M1_DEST + d, eg ? sf::MD_EG_TIME : b ? sf::MD_VOLUME : sf::MD_WAVE);
        set(sf::P_M1_AMT + d, 0.4f);
    }
}

struct Result { double avg, p99, max; };

using StageFn = int (*)(double*, const char**, int);

Result runCase(void* lib, int seconds, const char* name, int mode, StageFn stages) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no VSTPluginMain, or it made no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    if (mode >= 2) heavy(e);
    if (mode >= 4) hiRange(e, mode == 5);
    if (mode == 3) {
        e->setParameter(e, sf::P_GLIDE_MODE, norm(sf::P_GLIDE_MODE, sf::GL_ALWAYS));
        e->setParameter(e, sf::P_GLIDE, norm(sf::P_GLIDE, 0.04f));
    }
    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = {L.data(), R.data()};
    for (int b = 0; b < 32; ++b) e->processReplacing(e, nullptr, out, kBlock);   // the patch settles
    if (stages) {   // the stage counters start over here: only the timed blocks count
        double us[8];
        const char* names[8];
        stages(us, names, 8);
    }
    if (mode >= 1) midi(e, 0x90, 36, 110);
    if (mode >= 2) midi(e, 0x90, 43, 110);
    const int blocks = seconds * 44100 / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    int note = 0;
    for (int b = 0; b < blocks; ++b) {
        if (mode == 3 && b % 17 == 0) {   // every ~50 ms: a new pair of keys
            midi(e, 0x80, static_cast<uint8_t>(36 + note % 12), 0);
            midi(e, 0x80, static_cast<uint8_t>(43 + note % 12), 0);
            ++note;
            midi(e, 0x90, static_cast<uint8_t>(36 + note % 12), 100);
            midi(e, 0x90, static_cast<uint8_t>(43 + note % 12), 100);
        }
        const double t0 = cpuUs();
        e->processReplacing(e, nullptr, out, kBlock);
        t[static_cast<size_t>(b)] = cpuUs() - t0;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    const char* verdict = r.p99 <= 15.0 && r.max <= 50.0 ? "PASS" : r.p99 <= 35.0 && r.max <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-28s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", name, r.avg, r.p99, r.max, verdict);
    return r;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
    }
    if (cpu >= 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders, nothing saved.
    setenv("SF_PRESET_ROOTS", "/nonexistent-sfbench", 1);
    setenv("SF_DATA_DIR", "", 1);
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto stages = reinterpret_cast<StageFn>(dlsym(lib, "SubForceStageTimes"));
    std::printf("%s, %d s per case, %s\n", argv[1], seconds, stages ? "profiling build" : "plain build");
    const char* names[] = {"idle (no note)", "Init, one held note", "heavy patch, Duo", "heavy, retrig + glide 50 ms",
                           "heavy, both busses Hi range", "heavy, EG Time LFO + Hi FM"};
    bool fail = false;
    for (int mode = 0; mode < 6; ++mode) {
        double us[8] = {};
        const char* stageNames[8] = {};
        const Result r = runCase(lib, seconds, names[mode], mode, stages);
        fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0);
        if (stages) {
            const int n = stages(us, stageNames, 8);
            const int blocks = seconds * 44100 / kBlock;
            std::printf("      per block:");
            for (int k = 0; k < n; ++k) std::printf("  %s %.1f us", stageNames[k], us[k] / blocks);
            std::printf("\n");
        }
    }
    dlclose(lib);
    return fail ? 1 : 0;
}
