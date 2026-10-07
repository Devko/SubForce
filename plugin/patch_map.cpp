#include "patch_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sf {

// Option lists in surface.py against the engine's enums.
static_assert(kNumOctaves == kOctaveMax - kOctaveMin + 1, "surface.py OCTAVES must match dsp/synth.h");
static_assert(kNumSlopes == SL_24 + 1, "surface.py SLOPES must match dsp/synth.h Slope");
static_assert(kNumModSources == MS_COUNT && kNumModDests == MD_COUNT && kNumModControls == MC_COUNT &&
                  kNumSyncDivisions == kNumSyncDivs,
              "surface.py mod lists must match dsp/mod.h");
static_assert(PARAM_INFO[P_KMODE].nopts == KM_DUO + 1 && PARAM_INFO[P_PRIO].nopts == PR_HIGH + 1 &&
                  PARAM_INFO[P_TRIG].nopts == TR_SINGLE + 1 && PARAM_INFO[P_GLIDE_MODE].nopts == GL_LEGATO + 1 &&
                  PARAM_INFO[P_GLIDE_TYPE].nopts == GT_EXP + 1 && PARAM_INFO[P_GLIDE_DEST].nopts == OD_OSC2 + 1 &&
                  PARAM_INFO[P_M1_PDEST].nopts == OD_OSC2 + 1 && PARAM_INFO[P_SUB_OCT].nopts == SO_TWO + 1,
              "surface.py keyboard / glide / destination lists must match dsp/synth.h");
static_assert(PARAM_INFO[P_M1_SYNC].nopts == RM_COUNT && PARAM_INFO[P_M2_SYNC].nopts == RM_COUNT,
              "surface.py RATE_MODES must match dsp/mod.h RateMode");

// patchFromParams walks envelope 2 and mod bus 2 at a fixed offset from 1: every member of the
// second block must be the first's, in the same order ("fe_a" ~ "ae_a": the part after '_').
namespace {
constexpr const char* suffix(const char* k) {
    while (*k && *k != '_') ++k;
    return k;
}
constexpr bool same(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
constexpr bool sameBlock(int first, int other, int count) {
    for (int k = 0; k < count; ++k)
        if (!same(suffix(PARAM_INFO[first + k].key), suffix(PARAM_INFO[other + k].key))) return false;
    return true;
}
} // namespace
static_assert(sameBlock(P_FE_DLY, P_AE_DLY, P_FE_RESET - P_FE_DLY + 1), "amp EG params must mirror the filter EG's");
static_assert(sameBlock(P_M1_SRC, P_M2_SRC, P_M1_TRIG - P_M1_SRC + 1), "mod bus 2 params must mirror bus 1's");
static_assert(sameBlock(P_M1_WHEEL, P_M2_WHEEL, P_M1_AT - P_M1_WHEEL + 1), "bus 2's depth amounts must mirror bus 1's");

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;   // NaN-safe (std::clamp passes NaN through)
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;       // 0..hi, fine near 0 (times that may be 0)
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = v > 0.0f && s.hi > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Lin:
        case Curve::Int:
        case Curve::Enum: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    return std::isfinite(n) ? std::clamp(n, 0.0f, 1.0f) : 0.0f;   // values come from saved state text too
}

std::string waveName(float w) {
    const float m = std::clamp(w, 0.0f, 1.0f) * 3.0f;
    char b[32];
    if (m < 1.0f) {   // triangle -> saw
        if (m < 0.02f) return "Triangle";
        if (m > 0.98f) return "Saw";
        std::snprintf(b, sizeof b, "Tri-Saw %.0f%%", m * 100.0f);
    } else if (m < 2.0f) {   // saw -> square
        if (m < 1.02f) return "Saw";
        if (m > 1.98f) return "Square";
        std::snprintf(b, sizeof b, "Saw-Sqr %.0f%%", (m - 1.0f) * 100.0f);
    } else {   // square -> narrow pulse: the width
        const float width = 50.0f - (m - 2.0f) * (50.0f - 100.0f * kMinPulse);
        if (m < 2.02f) return "Square";
        std::snprintf(b, sizeof b, "Pulse %.0f%%", width);
    }
    return b;
}

std::string lfoHzText(float hz) {
    char b[32];
    std::snprintf(b, sizeof b, hz < 0.995f ? "%.2f Hz" : (hz < 9.95f ? "%.1f Hz" : "%.0f Hz"), hz);
    return b;
}

std::string paramDisplay(int id, float n) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, n);
    char b[32];
    switch (PARAM_SPECS[id].fmt) {
        case Fmt::Enum: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::Bipolar:
            std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f);
            break;
        // Unit changes where the rounded text would reach the next unit ("1000 Hz" is "1.00 kHz").
        case Fmt::Hz:
            if (v < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 9995.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        case Fmt::Time:
            if (v < 0.00005f) return "0 ms";
            if (v < 0.00995f) std::snprintf(b, sizeof b, "%.1f ms", v * 1000.0f);
            else if (v < 0.9995f) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0f);
            else std::snprintf(b, sizeof b, "%.2f s", v);
            break;
        case Fmt::Semi: std::snprintf(b, sizeof b, v == 0.0f ? "0 st" : "%+.0f st", v); break;
        case Fmt::SemiFine:
            if (std::fabs(v) < 0.005f) return "0.00 st";
            std::snprintf(b, sizeof b, "%+.2f st", v);
            break;
        case Fmt::Count: std::snprintf(b, sizeof b, "%.0f", v); break;
        case Fmt::Range: std::snprintf(b, sizeof b, "%.0f st", v); break;   // a bend range: no sign
        case Fmt::Db:
            if (v <= -59.5f) return "-inf dB";
            std::snprintf(b, sizeof b, "%.1f dB", std::fabs(v) < 0.05f ? 0.0f : v);   // never "-0.0 dB"
            break;
        case Fmt::LfoHz: return lfoHzText(v);
        case Fmt::BeatHz:
            if (std::fabs(v) < 0.005f) return "0.00 Hz";
            std::snprintf(b, sizeof b, "%+.2f Hz", v);
            break;
        case Fmt::Wave: return waveName(v);
        case Fmt::EnvAmt: {
            const float oct = envSemis(v) / 12.0f;
            if (std::fabs(oct) < 0.005f) return "0 oct";
            std::snprintf(b, sizeof b, "%+.2f oct", oct);
            break;
        }
        case Fmt::ModPitch: {
            const float st = v * std::fabs(v) * kModPitchRange;
            if (std::fabs(st) < 0.005f) return "0 st";
            std::snprintf(b, sizeof b, std::fabs(st) < 1.0f ? "%+.2f st" : "%+.1f st", st);
            break;
        }
        case Fmt::ModCut: {
            const float oct = v * std::fabs(v) * kModCutoffRange / 12.0f;
            if (std::fabs(oct) < 0.005f) return "0 oct";
            std::snprintf(b, sizeof b, "%+.2f oct", oct);
            break;
        }
        case Fmt::Noise:   // white .. pink (the original's) at the middle .. dark
            if (v < 0.005f) return "White";
            if (std::fabs(v - 0.5f) < 0.005f) return "Pink";
            if (v < 0.5f) std::snprintf(b, sizeof b, "Pink %.0f%%", 200.0f * v);
            else std::snprintf(b, sizeof b, "Dark %.0f%%", 200.0f * v - 100.0f);
            break;
        default: return {};
    }
    return b;
}

Patch patchFromParams(const float* norm) {
    auto V = [norm](int id) { return paramValue(id, norm[id]); };
    auto I = [&V](int id) { return static_cast<int>(V(id)); };
    Patch p;
    p.volumeDb = V(P_VOLUME);
    p.osc[0].octave = I(P_O1_OCT) + kOctaveMin;
    p.osc[0].wave = V(P_O1_WAVE);
    p.osc[1].octave = I(P_O2_OCT) + kOctaveMin;
    p.osc[1].wave = V(P_O2_WAVE);
    p.osc2Semis = V(P_O2_FREQ);
    p.beatHz = V(P_O2_BEAT);
    p.sync = I(P_O2_SYNC) != 0;
    p.subOctave = I(P_SUB_OCT);
    p.kbReset = I(P_KB_RESET) != 0;
    p.drift = V(P_DRIFT);
    p.mixOsc1 = V(P_MIX_O1);
    p.mixSub = V(P_MIX_SUB);
    p.mixOsc2 = V(P_MIX_O2);
    p.mixNoise = V(P_MIX_NOISE);
    p.mixFeedback = V(P_MIX_FB);
    p.noiseColor = V(P_NOISE_COLOR);
    p.cutoffHz = V(P_F_CUT);
    p.res = V(P_F_RES);
    p.drive = V(P_F_DRIVE);
    p.slope = I(P_F_SLOPE);
    p.envAmount = V(P_F_ENV);
    p.keyTrack = V(P_F_KB);
    for (int e = 0; e < 2; ++e) {
        const int d = e * (P_AE_DLY - P_FE_DLY);
        EnvPatch& x = e == 0 ? p.fenv : p.aenv;
        x.delay = V(P_FE_DLY + d);
        x.attack = V(P_FE_A + d);
        x.hold = V(P_FE_HOLD + d);
        x.decay = V(P_FE_D + d);
        x.sustain = V(P_FE_S + d);
        x.release = V(P_FE_R + d);
        x.vel = V(P_FE_VEL + d);
        x.kb = V(P_FE_KB + d);
        x.loop = I(P_FE_LOOP + d) != 0;
        x.reset = I(P_FE_RESET + d) != 0;
    }
    for (int b = 0; b < 2; ++b) {
        const int d = b * (P_M2_SRC - P_M1_SRC);
        ModPatch& m = p.mod[b];
        m.src = I(P_M1_SRC + d);
        const int rateMode = I(P_M1_SYNC + d);
        m.sync = rateMode == RM_SYNC;
        m.hi = rateMode == RM_HI;
        m.rateHz = V(P_M1_RATE + d);
        m.div = I(P_M1_DIV + d);
        m.pitch = V(P_M1_PITCH + d);
        m.pitchDest = I(P_M1_PDEST + d);
        m.filter = V(P_M1_FILTER + d);
        m.dest = I(P_M1_DEST + d);
        m.amount = V(P_M1_AMT + d);
        m.control = I(P_M1_CTL + d);
        m.retrig = I(P_M1_TRIG + d) != 0;
        const int a = b * (P_M2_WHEEL - P_M1_WHEEL);   // the depth amounts (appended in 0.0.3)
        m.wheel = V(P_M1_WHEEL + a);
        m.vel = V(P_M1_VEL + a);
        m.at = V(P_M1_AT + a);
    }
    p.keyMode = I(P_KMODE);
    p.priority = I(P_PRIO);
    p.trigger = I(P_TRIG);
    p.glideMode = I(P_GLIDE_MODE);
    p.glideType = I(P_GLIDE_TYPE);
    p.glideTime = V(P_GLIDE);
    p.glideDest = I(P_GLIDE_DEST);
    p.bendUp = V(P_BEND_UP);
    p.bendDown = V(P_BEND_DN);
    return p;
}

} // namespace sf
