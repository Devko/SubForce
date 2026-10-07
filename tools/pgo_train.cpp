// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the
// plugin is linked in and plays a spread of patches through VSTPluginMain under qemu-arm, the
// way MPC drives it: every slope and wave region, sync, the sub, noise, feedback, drive and
// resonance, Mono and Duo, the trigger and glide modes, both busses with every source and
// destination (free, synced and Hi range, key tracked), the beat frequency, the depth amounts, the
// envelopes' attack curves, latch and sync, osc 2's keys, the bend's oscillators, gated glide, and the
// factory presets. The profile only steers the compiler (which paths are hot); what the
// trainer leaves out is still optimised as usual (-fprofile-partial-training).
#include "../dsp/synth.h"
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
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

// A short phrase: a few legato and detached notes, the wheel, a release.
void phrase(AEffect* e, std::vector<float>& L, std::vector<float>& R, int blocks) {
    float* out[2] = {L.data(), R.data()};
    static const int notes[] = {36, 43, 48, 41};
    for (int k = 0; k < 4; ++k) {
        midi(e, 0x90, static_cast<uint8_t>(notes[k]), static_cast<uint8_t>(70 + 15 * k));
        if (k == 2) midi(e, 0xB0, 1, 100);
        for (int b = 0; b < blocks; ++b) e->processReplacing(e, nullptr, out, 128);
        if (k != 1) midi(e, 0x80, static_cast<uint8_t>(notes[k]), 0);
    }
    midi(e, 0x80, static_cast<uint8_t>(notes[1]), 0);
    midi(e, 0xB0, 1, 0);
    for (int b = 0; b < blocks; ++b) e->processReplacing(e, nullptr, out, 128);
}

} // namespace

int main() {
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    std::vector<float> L(128), R(128);
    int patches = 0;
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
    // The common ground: every slope and wave region, with and without sync, sub and drive.
    for (int slope = 0; slope < 4; ++slope)
        for (float wave : {0.1f, 0.4f, 0.75f, 1.0f}) {
            set(sf::P_F_SLOPE, slope);
            set(sf::P_O1_WAVE, wave);
            set(sf::P_O2_WAVE, 1.0f - wave);
            set(sf::P_MIX_O2, slope % 2 ? 0.7f : 0.0f);
            set(sf::P_O2_SYNC, wave > 0.5f ? 1 : 0);
            set(sf::P_MIX_SUB, slope > 1 ? 0.5f : 0.0f);
            set(sf::P_F_DRIVE, 0.25f * slope);
            set(sf::P_F_RES, 0.2f * slope);
            phrase(e, L, R, 30);
            ++patches;
        }
    // Noise, feedback, Duo, the trigger and glide modes, the busses: every source and destination
    // on one bus or the other, free, synced and in Hi range.
    set(sf::P_MIX_NOISE, 0.3f);
    set(sf::P_MIX_FB, 0.4f);
    for (int k = 0; k < sf::MS_COUNT; ++k) {
        set(sf::P_KMODE, k % 2);
        set(sf::P_TRIG, (k / 2) % 2);
        set(sf::P_GLIDE_TYPE, k % 3);
        set(sf::P_O2_BEAT, k % 3 ? 1.5f : 0.0f);
        set(sf::P_O2_KB, k % 4);
        set(sf::P_BEND_DEST, (k / 2) % 4);
        set(sf::P_GLIDE_MODE, k % 5);
        for (int env = 0; env < 2; ++env) {   // the envelopes' 0.0.4 options
            const int d = env * (sf::P_AE_EXP - sf::P_FE_EXP);
            set(sf::P_FE_EXP + d, (k + env) % 2);
            set(sf::P_FE_LATCH + d, k % 7 == 3 + env ? 1 : 0);
            set(sf::P_FE_SYNC + d, k % 3 == env ? 1 + (k % sf::kNumSyncDivs) : 0);
        }
        for (int b = 0; b < 2; ++b) {
            const int d = b * (sf::P_M2_SRC - sf::P_M1_SRC), a = b * (sf::P_M2_WHEEL - sf::P_M1_WHEEL);
            set(sf::P_M1_SRC + d, (k + 5 * b) % sf::MS_COUNT);
            set(sf::P_M1_DEST + d, (k + 10 * b) % sf::MD_COUNT);
            set(sf::P_M1_PITCH + d, 0.2f);
            set(sf::P_M1_FILTER + d, 0.4f);
            set(sf::P_M1_AMT + d, 0.5f);
            set(sf::P_M1_SYNC + d, (k + b) % sf::RM_COUNT);
            set(sf::P_M1_RATE + d, (k + b) % sf::RM_COUNT == sf::RM_HI ? 30.0f : 4.0f);
            set(sf::P_M1_CTL + d, (k + b) % sf::MC_COUNT);
            set(sf::P_M1_VEL + a, k % 2 ? 0.5f : 0.0f);
            set(b ? sf::P_M2_KBT : sf::P_M1_KBT, k % 3 ? 1.0f : 0.0f);
        }
        phrase(e, L, R, 30);
        ++patches;
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    // The factory presets, as users will mostly play them: each on a fresh instance (a project
    // chunk only sets what it lists; a preset means everything else at its default).
    for (int i = 0; i < sf::kNumFactoryPresets; ++i) {
        AEffect* f = VSTPluginMain(master);
        f->dispatcher(f, vst::effOpen, 0, 0, nullptr, 0.0f);
        const std::string text = sf::kFactoryPresets[i].text;
        f->dispatcher(f, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.data()), 0.0f);
        phrase(f, L, R, 25);
        f->dispatcher(f, vst::effClose, 0, 0, nullptr, 0.0f);
        ++patches;
    }
    std::printf("pgo trainer: %d patches\n", patches);
    return 0;
}
