// SubForce's demo track: a melodic-techno build in F minor played through the built SubForce, layer by
// layer (bass sequence, pluck, gliding lead, pad), each entering clean and then through its own
// EffectForce (an insert on that part), level-matched; then all together, a breakdown with a riser, a
// drop on an impact (both SubForce too) and an ending. A mix bus on the master (EQ, glue), mastered at
// -10 LUFS. No drums: the synth on its own. Written from what melodic-techno production tutorials
// teach, in our own parts: a bass on the chord roots off the beat with 1-5-octave jumps and velocity
// accents, a sparse arp the delay completes, cutoffs that never stand still, several moves stacked at
// the drop, generous delays (thrown at phrase ends), high-passed returns, a pump for a sidechain.
// Each plugin is dlopen()ed and played as MPC plays it (128-frame blocks, MIDI with sample offsets,
// VstTimeInfo with the transport running); the arrangement is song() at the bottom. Two passes: the
// first measures each preset's clean and effected halves, the second plays the effected half at the
// clean half's loudness (an effect should sound different, not just louder). Writes:
//
//   <out>/mix.wav       the track, mastered (-10 LUFS, peaks under -1 dBFS), 16-bit stereo 44.1 kHz
//   <out>/state.jsonl   every watched instance's parameters, value and display text, per video frame
//                       (30 fps), as changes: {"f": frame, "i": instance, "v": {"index": [value, text]}}
//   <out>/cues.jsonl    what the video shows when: sections, pages, captions, taps (time in seconds)
//
//   showreel <out dir>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr double kSr = 44100.0;
constexpr int kBlock = 128;
constexpr double kFps = 30.0;
constexpr double kBpm = 122.0;
constexpr double kSpb = kSr * 60.0 / kBpm;   // samples per beat
const std::string kDev = "/mnt/d/DEV/";

// Standard VST 2.4 values the vendored headers leave out.
constexpr intptr_t kMasterVersion = 1;
constexpr int32_t kTransportChanged = 1, kBarsValid = 1 << 11, kTimeSigValid = 1 << 13;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == kMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "can't read %s\n", path.c_str());
        std::exit(1);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- just enough JSON for params.json ----------------------------------------------------------
struct J {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } k = Null;
    double d = 0.0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* at(const std::string& key) const {
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JsonParser {
    const char* p;
    void ws() {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    }
    std::string str() {
        ++p;
        std::string r;
        while (*p && *p != '"') {
            if (*p != '\\') {
                r += *p++;
                continue;
            }
            ++p;
            if (*p == 'u') {
                const unsigned v = static_cast<unsigned>(std::strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
                p += 5;
                if (v < 0x80) {
                    r += static_cast<char>(v);
                } else if (v < 0x800) {
                    r += static_cast<char>(0xC0 | (v >> 6));
                    r += static_cast<char>(0x80 | (v & 0x3F));
                } else {
                    r += static_cast<char>(0xE0 | (v >> 12));
                    r += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                    r += static_cast<char>(0x80 | (v & 0x3F));
                }
                continue;
            }
            r += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            ++p;
        }
        ++p;
        return r;
    }
    J val() {
        ws();
        J j;
        if (*p == '{') {
            j.k = J::Obj;
            ++p;
            ws();
            if (*p == '}') return ++p, j;
            for (;;) {
                ws();
                std::string key = str();
                ws();
                ++p;   // ':'
                J v = val();
                j.o.emplace_back(key, std::move(v));
                ws();
                if (*p++ != ',') break;
            }
        } else if (*p == '[') {
            j.k = J::Arr;
            ++p;
            ws();
            if (*p == ']') return ++p, j;
            for (;;) {
                j.a.push_back(val());
                ws();
                if (*p++ != ',') break;
            }
        } else if (*p == '"') {
            j.k = J::Str;
            j.s = str();
        } else if (!std::strncmp(p, "true", 4)) {
            j.k = J::Bool, j.d = 1, p += 4;
        } else if (!std::strncmp(p, "false", 5)) {
            j.k = J::Bool, p += 5;
        } else if (!std::strncmp(p, "null", 4)) {
            p += 4;
        } else {
            char* e = nullptr;
            j.k = J::Num;
            j.d = std::strtod(p, &e);
            p = e;
        }
        return j;
    }
};

std::string jsonEscape(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\', r += c;
        else if (static_cast<unsigned char>(c) < 0x20) r += ' ';
        else r += c;
    }
    return r;
}

// --- plugins and instances ---------------------------------------------------------------------
struct Lib {
    std::string name;
    AEffect* (*entry)(audioMasterCallback) = nullptr;
    std::vector<std::string> keys;
    std::vector<std::vector<std::string>> options;
    std::map<std::string, int> index;
};

std::unique_ptr<Lib> loadLib(const std::string& name, const std::string& so, const std::string& paramsJson) {
    auto lib = std::make_unique<Lib>();
    lib->name = name;
    void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "dlopen %s: %s\n", so.c_str(), dlerror());
        std::exit(1);
    }
    lib->entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(h, "VSTPluginMain"));
    const std::string text = slurp(paramsJson);
    JsonParser jp{text.c_str()};
    const J root = jp.val();
    for (const J& p : root.at("params")->a) {
        lib->index[p.at("key")->s] = static_cast<int>(lib->keys.size());
        lib->keys.push_back(p.at("key")->s);
        std::vector<std::string> opts;
        if (const J* o = p.at("options"))
            for (const J& x : o->a) {
                char b[32];
                std::snprintf(b, sizeof b, "%g", x.d);
                opts.push_back(x.k == J::Str ? x.s : b);
            }
        lib->options.push_back(opts);
    }
    return lib;
}

struct Inst {
    std::string name;
    Lib* lib = nullptr;
    AEffect* e = nullptr;
    double gain = 1.0, pan = 0.0;   // pan -1..1
    bool watched = false;
    std::vector<VstMidiEvent> midi;   // this block's
    std::vector<float> lastVal;
    std::vector<std::string> lastText;
    double sumSq = 0.0, peak = 0.0;   // for the level report
    size_t loudBlocks = 0;
    // An insert (EffectForce on this part): the part's sound goes through it, mixed dry/effected by
    // `wet`, which glides to `wetTo` at `wetStep` per sample; `fxGain` levels the effected sound.
    Inst* insert = nullptr;
    bool isInsert = false;
    float wet = 0.0f, wetTo = 0.0f, wetStep = 1.0f;
    double fxGain = 1.0;

    int at(const std::string& key) const {
        const auto it = lib->index.find(key);
        if (it == lib->index.end()) {
            std::fprintf(stderr, "%s: no parameter %s\n", name.c_str(), key.c_str());
            std::exit(1);
        }
        return it->second;
    }
    void set(const std::string& key, float v) { e->setParameter(e, at(key), v); }
    float get(const std::string& key) const { return e->getParameter(e, at(key)); }
    std::string text(int i) const {
        char b[256] = {0};
        e->dispatcher(e, vst::effGetParamDisplay, i, 0, b, 0.0f);
        return b;
    }
    // An option parameter's normalized value for one of its options, by name.
    float option(const std::string& key, const std::string& opt) const {
        const auto& opts = lib->options[static_cast<size_t>(at(key))];
        for (size_t k = 0; k < opts.size(); ++k)
            if (opts[k] == opt) return opts.size() > 1 ? static_cast<float>(k) / static_cast<float>(opts.size() - 1) : 0.0f;
        std::fprintf(stderr, "%s: %s has no option %s\n", name.c_str(), key.c_str(), opt.c_str());
        std::exit(1);
    }
};

// The first number in a display text, kHz and ms read as Hz and ms ("1.2 kHz" -> 1200).
bool numberIn(const std::string& t, double& out) {
    const char* s = t.c_str();
    while (*s && !(std::isdigit(static_cast<unsigned char>(*s)) || ((*s == '-' || *s == '+' || *s == '.') && std::isdigit(static_cast<unsigned char>(s[1])))))
        ++s;
    if (!*s) return false;
    char* e = nullptr;
    out = std::strtod(s, &e);
    while (*e == ' ') ++e;
    if (*e == 'k' || *e == 'K') out *= 1000.0;
    if (*e == 's' && e[1] == '\0') out *= 1000.0;   // "1.20 s" against "995 ms": times in ms
    return true;
}

// --- the session: instances, the timeline, the mix ---------------------------------------------
struct Ramp {
    Inst* in;
    int idx;
    double b0, b1;
    float v0, v1;
    bool done = false;
};

struct Session {
    std::vector<std::unique_ptr<Lib>> libs;
    std::vector<std::unique_ptr<Inst>> insts;
    Inst* fx = nullptr;   // on the master
    struct Event {
        double beat;
        int seq;
        std::function<void(int)> run;   // the sample offset in the block
    };
    std::vector<Event> events;
    std::vector<Ramp> ramps;
    std::vector<std::string> cues;
    int seq = 0;

    Lib* lib(const std::string& name) {
        for (auto& l : libs)
            if (l->name == name) return l.get();
        return nullptr;
    }
    Inst* add(const std::string& name, const std::string& libName, const std::string& presetPath, double gainDb, double pan = 0.0) {
        auto in = std::make_unique<Inst>();
        in->name = name;
        in->lib = lib(libName);
        in->e = in->lib->entry(master);
        in->e->dispatcher(in->e, vst::effOpen, 0, 0, nullptr, 0.0f);
        in->e->dispatcher(in->e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
        if (!presetPath.empty()) {
            // Loaded as a project that came from the factory preset, so the pages show its name:
            // ".../04_Pad/01_Evolving_Pad.pfp" -> "preset=builtin:Evolving Pad".
            std::string stem = presetPath.substr(presetPath.rfind('/') + 1);
            stem = stem.substr(stem.find('_') + 1, stem.rfind('.') - stem.find('_') - 1);
            std::replace(stem.begin(), stem.end(), '_', ' ');
            const std::string text = slurp(kDev + presetPath) + "\npreset=builtin:" + stem + "\n";
            in->e->dispatcher(in->e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.c_str()), 0.0f);
        }
        in->gain = std::pow(10.0, gainDb / 20.0);
        in->pan = pan;
        const std::string plug = libName == "pf" ? "PolyForce" : libName == "sf" ? "SubForce" : "EffectForce";
        cue(0.0, "{\"type\": \"inst\", \"i\": \"" + name + "\", \"plug\": \"" + plug + "\"}");
        insts.push_back(std::move(in));
        return insts.back().get();
    }
    // An instance from a state text (a chain written in the song, not a factory preset).
    Inst* addState(const std::string& name, const std::string& libName, const std::string& text, double gainDb) {
        Inst* in = add(name, libName, "", gainDb);
        in->e->dispatcher(in->e, vst::effSetChunk, 0, static_cast<intptr_t>(text.size()), const_cast<char*>(text.c_str()), 0.0f);
        return in;
    }
    Inst* addInsertState(Inst* host, const std::string& name, const std::string& text) {
        Inst* fxIn = addState(name, "ef", text, 0.0);
        fxIn->isInsert = true;
        fxIn->watched = true;
        host->insert = fxIn;
        return fxIn;
    }
    // EffectForce with `preset` as an insert on `host`: watched, not mixed on its own.
    Inst* addInsert(Inst* host, const std::string& name, const std::string& preset) {
        Inst* fxIn = add(name, "ef", preset, 0.0);
        fxIn->isInsert = true;
        fxIn->watched = true;
        host->insert = fxIn;
        return fxIn;
    }
    // From `beat`, the part's effected share glides to `to` over `beats`.
    void wet(Inst* in, double beat, float to, double beats) {
        at(beat, [in, to, beats](int) {
            in->wetTo = to;
            in->wetStep = static_cast<float>(1.0 / std::max(beats * kSpb, 1.0));
        });
    }
    void gainAt(Inst* in, double beat, double db) {
        at(beat, [in, db](int) { in->gain = std::pow(10.0, db / 20.0); });
    }
    Inst* find(const std::string& name) {
        for (auto& in : insts)
            if (in->name == name) return in.get();
        std::fprintf(stderr, "no instance %s\n", name.c_str());
        std::exit(1);
    }

    void at(double beat, std::function<void(int)> fn) { events.push_back({beat, seq++, std::move(fn)}); }
    void note(Inst* in, double beat, double len, int key, int vel) {
        auto ev = [in](uint8_t st, uint8_t k, uint8_t v) {
            return [in, st, k, v](int off) {
                VstMidiEvent m{};
                m.type = vst::kVstMidiType;
                m.byteSize = sizeof m;
                m.deltaFrames = off;
                m.midiData[0] = static_cast<char>(st);
                m.midiData[1] = static_cast<char>(k);
                m.midiData[2] = static_cast<char>(v);
                in->midi.push_back(m);
            };
        };
        at(beat, ev(0x90, static_cast<uint8_t>(key), static_cast<uint8_t>(vel)));
        at(beat + len, ev(0x80, static_cast<uint8_t>(key), 0));
    }
    void cc(Inst* in, double beat, int ctl, int v) {
        at(beat, [in, ctl, v](int off) {
            VstMidiEvent m{};
            m.type = vst::kVstMidiType;
            m.byteSize = sizeof m;
            m.deltaFrames = off;
            m.midiData[0] = static_cast<char>(0xB0);
            m.midiData[1] = static_cast<char>(ctl);
            m.midiData[2] = static_cast<char>(v);
            in->midi.push_back(m);
        });
    }
    void bend(Inst* in, double beat, float v) {   // -1..1
        const int x = std::clamp(static_cast<int>(std::lround((v + 1.0f) * 8192.0f)), 0, 16383);
        at(beat, [in, x](int off) {
            VstMidiEvent m{};
            m.type = vst::kVstMidiType;
            m.byteSize = sizeof m;
            m.deltaFrames = off;
            m.midiData[0] = static_cast<char>(0xE0);
            m.midiData[1] = static_cast<char>(x & 0x7F);
            m.midiData[2] = static_cast<char>(x >> 7);
            in->midi.push_back(m);
        });
    }
    void set(Inst* in, double beat, const std::string& key, float v) {
        const int i = in->at(key);
        at(beat, [in, i, v](int) { in->e->setParameter(in->e, i, v); });
    }
    void opt(Inst* in, double beat, const std::string& key, const std::string& option) { set(in, beat, key, in->option(key, option)); }
    // A tap on a tile or a button: MPC sends a 1 (a button springs back on its own).
    void tap(Inst* in, double beat, const std::string& key) {
        set(in, beat, key, 1.0f);
        cue(beat, "{\"type\": \"tap\", \"i\": \"" + in->name + "\", \"key\": \"" + key + "\"}");
    }
    void ramp(Inst* in, double b0, double b1, const std::string& key, float v0, float v1) { ramps.push_back({in, in->at(key), b0, b1, v0, v1}); }

    void cue(double beat, const std::string& json) {
        char t[48];
        std::snprintf(t, sizeof t, "{\"t\": %.4f, \"beat\": %.4f, ", beat * 60.0 / kBpm, beat);
        cues.push_back(t + json.substr(1));
    }
    void page(double beat, Inst* in, const std::string& tab) {
        cue(beat, "{\"type\": \"page\", \"i\": \"" + in->name + "\", \"tab\": \"" + tab + "\"}");
    }
    void caption(double beat, const std::string& head, const std::string& body = "") {
        cue(beat, "{\"type\": \"caption\", \"head\": \"" + jsonEscape(head) + "\", \"body\": \"" + jsonEscape(body) + "\"}");
    }
    void section(double beat, const std::string& name) { cue(beat, "{\"type\": \"section\", \"name\": \"" + name + "\"}"); }

    // The normalized value whose display reads closest to `target` (a scratch instance is swept).
    float valueFor(Inst* in, const std::string& key, double target) {
        AEffect* e = in->lib->entry(master);
        e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
        const int i = in->at(key);
        float best = 0.0f;
        double bestErr = 1e30;
        for (int k = 0; k <= 2000; ++k) {
            const float v = static_cast<float>(k) / 2000.0f;
            e->setParameter(e, i, v);
            char b[256] = {0};
            e->dispatcher(e, vst::effGetParamDisplay, i, 0, b, 0.0f);
            double x;
            if (numberIn(b, x) && std::fabs(x - target) < bestErr) bestErr = std::fabs(x - target), best = v;
        }
        e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
        return best;
    }

    void logState(std::ofstream& out, long frame) {
        for (auto& in : insts) {
            if (!in->watched) continue;
            const int n = static_cast<int>(in->lib->keys.size());
            if (in->lastVal.empty()) in->lastVal.assign(static_cast<size_t>(n), -1.0f), in->lastText.assign(static_cast<size_t>(n), "\x01");
            std::string body;
            for (int i = 0; i < n; ++i) {
                const float v = in->e->getParameter(in->e, i);
                const std::string t = in->text(i);
                if (v == in->lastVal[static_cast<size_t>(i)] && t == in->lastText[static_cast<size_t>(i)]) continue;
                in->lastVal[static_cast<size_t>(i)] = v;
                in->lastText[static_cast<size_t>(i)] = t;
                char b[64];
                std::snprintf(b, sizeof b, "%s\"%d\": [%.5f, \"", body.empty() ? "" : ", ", i, v);
                body += b + jsonEscape(t) + "\"]";
            }
            if (!body.empty()) out << "{\"f\": " << frame << ", \"i\": \"" << in->name << "\", \"v\": {" << body << "}}\n";
        }
    }

    // Plays the timeline to `endBeat`; returns the master, stereo.
    void run(double endBeat, std::ofstream& state, std::vector<float>& outL, std::vector<float>& outR) {
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.beat < b.beat; });
        std::stable_sort(ramps.begin(), ramps.end(), [](const Ramp& a, const Ramp& b) { return a.b0 < b.b0; });
        // Before the first beat: the transport stopped, silence played until PolyForce's tables are in.
        g_time.sampleRate = kSr;
        g_time.tempo = kBpm;
        g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
        g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | kTimeSigValid | kBarsValid;
        std::vector<float> L(kBlock), R(kBlock), mL(kBlock), mR(kBlock);
        for (int b = 0; b < 600; ++b) {
            bool loading = false;
            for (auto& in : insts) {
                std::fill(L.begin(), L.end(), 0.0f);
                std::fill(R.begin(), R.end(), 0.0f);
                float* o[2] = {L.data(), R.data()};
                in->e->processReplacing(in->e, o, o, kBlock);
                if (in->lib->name == "pf")
                    for (const char* k : {"o1_table", "o2_table"})
                        if (in->text(in->at(k)).rfind("LOADING", 0) == 0) loading = true;
            }
            if (!loading && b > 40) break;
            usleep(2000);
        }
        g_time.flags |= vst::kVstTransportPlaying | kTransportChanged;
        const size_t total = static_cast<size_t>(endBeat * kSpb) / kBlock * kBlock;
        outL.assign(total, 0.0f);
        outR.assign(total, 0.0f);
        size_t next = 0;
        long frame = 0;
        for (size_t s = 0; s < total; s += kBlock) {
            const double b0 = static_cast<double>(s) / kSpb, b1 = static_cast<double>(s + kBlock) / kSpb;
            while (next < events.size() && events[next].beat < b1) {
                const int off = std::clamp(static_cast<int>(std::lround(events[next].beat * kSpb)) - static_cast<int>(s), 0, kBlock - 1);
                events[next].run(off);
                ++next;
            }
            for (Ramp& r : ramps) {
                if (r.done || b0 < r.b0) continue;
                const double t = std::clamp((b0 - r.b0) / std::max(r.b1 - r.b0, 1e-9), 0.0, 1.0);
                r.in->e->setParameter(r.in->e, r.idx, r.v0 + static_cast<float>(t) * (r.v1 - r.v0));
                r.done = b0 >= r.b1;
            }
            while (static_cast<double>(frame) * kSr / kFps <= static_cast<double>(s)) logState(state, frame++);
            g_time.samplePos = static_cast<double>(s);
            g_time.ppqPos = b0;
            g_time.barStartPos = std::floor(b0 / 4.0) * 4.0;
            std::fill(mL.begin(), mL.end(), 0.0f);
            std::fill(mR.begin(), mR.end(), 0.0f);
            for (auto& in : insts) {
                if (in.get() == fx || in->isInsert) continue;
                if (!in->midi.empty()) {
                    std::vector<char> buf(sizeof(VstEvents) + in->midi.size() * sizeof(VstEvent*));
                    auto* evs = reinterpret_cast<VstEvents*>(buf.data());
                    evs->numEvents = static_cast<int32_t>(in->midi.size());
                    for (size_t k = 0; k < in->midi.size(); ++k) evs->events[k] = reinterpret_cast<VstEvent*>(&in->midi[k]);
                    in->e->dispatcher(in->e, vst::effProcessEvents, 0, 0, evs, 0.0f);
                    in->midi.clear();
                }
                float* o[2] = {L.data(), R.data()};
                in->e->processReplacing(in->e, nullptr, o, kBlock);
                if (Inst* fi = in->insert) {   // through its EffectForce, dry and effected mixed by `wet`
                    std::vector<float> dL(L), dR(R);
                    float* io[2] = {L.data(), R.data()};
                    fi->e->processReplacing(fi->e, io, io, kBlock);
                    for (int k = 0; k < kBlock; ++k) {
                        if (in->wet < in->wetTo) in->wet = std::min(in->wetTo, in->wet + in->wetStep);
                        else if (in->wet > in->wetTo) in->wet = std::max(in->wetTo, in->wet - in->wetStep);
                        const float w = in->wet, g = static_cast<float>(in->fxGain);
                        L[k] = dL[k] * (1.0f - w) + L[k] * g * w;
                        R[k] = dR[k] * (1.0f - w) + R[k] * g * w;
                    }
                }
                const double gl = in->gain * std::min(1.0, 1.0 - in->pan), gr = in->gain * std::min(1.0, 1.0 + in->pan);
                double sq = 0.0;
                for (int k = 0; k < kBlock; ++k) {
                    mL[k] += static_cast<float>(L[k] * gl);
                    mR[k] += static_cast<float>(R[k] * gr);
                    sq += (L[k] * L[k] + R[k] * R[k]) * in->gain * in->gain * 0.5;
                    in->peak = std::max(in->peak, std::fabs(L[k] * gl));
                }
                if (sq / kBlock > 1e-6) in->sumSq += sq / kBlock, ++in->loudBlocks;
            }
            if (fx) {
                float* io[2] = {mL.data(), mR.data()};
                fx->e->processReplacing(fx->e, io, io, kBlock);
            }
            std::copy(mL.begin(), mL.end(), outL.begin() + static_cast<long>(s));
            std::copy(mR.begin(), mR.end(), outR.begin() + static_cast<long>(s));
            g_time.flags &= ~kTransportChanged;
        }
    }
};

// --- mastering: BS.1770 loudness, a look-ahead peak limiter --------------------------------------
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

double integratedLufs(const std::vector<float>& L, const std::vector<float>& R) {
    auto shelf = [] {
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(M_PI * f0 / kSr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        return Biquad{(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                      2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    };
    auto hp = [] {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(M_PI * f0 / kSr), a0 = 1.0 + k / q + k * k;
        return Biquad{1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    };
    Biquad s[2] = {shelf(), shelf()}, h[2] = {hp(), hp()};
    std::vector<double> pw(L.size());
    for (size_t i = 0; i < L.size(); ++i) {
        const double l = h[0].run(s[0].run(L[i])), r = h[1].run(s[1].run(R[i]));
        pw[i] = l * l + r * r;
    }
    const size_t block = static_cast<size_t>(0.4 * kSr), hop = block / 4;
    std::vector<double> z;
    for (size_t a = 0; a + block <= pw.size(); a += hop) {
        double sum = 0.0;
        for (size_t i = a; i < a + block; ++i) sum += pw[i];
        z.push_back(sum / static_cast<double>(block));
    }
    auto loud = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-20)); };
    double sum = 0.0;
    int n = 0;
    for (double v : z)
        if (loud(v) > -70.0) sum += v, ++n;
    if (!n) return -100.0;
    const double rel = loud(sum / n) - 10.0;
    sum = 0.0, n = 0;
    for (double v : z)
        if (loud(v) > -70.0 && loud(v) > rel) sum += v, ++n;
    return n ? loud(sum / n) : -100.0;
}

void limit(std::vector<float>& L, std::vector<float>& R, double ceiling) {
    const size_t look = 64, n = L.size();
    const double release = std::exp(-1.0 / (0.08 * kSr));
    std::vector<double> need(n, 1.0);   // the gain each sample needs on its own
    for (size_t i = 0; i < n; ++i) {
        const double p = std::max(std::fabs(L[i]), std::fabs(R[i]));
        need[i] = p > ceiling ? ceiling / p : 1.0;
    }
    // The gain at i: the least needed within the look-ahead, reached smoothly, released slowly.
    double g = 1.0;
    std::vector<double> minAhead(n, 1.0);
    for (size_t i = n; i-- > 0;) {
        double m = need[i];
        for (size_t k = 1; k < look && i + k < n; k += 8) m = std::min(m, need[i + k]);
        minAhead[i] = m;
    }
    for (size_t i = 0; i < n; ++i) {
        const double target = minAhead[i];
        g = target < g ? g + (target - g) * 0.25 : target + (g - target) * release;
        g = std::min(g, need[i]);
        L[i] = static_cast<float>(L[i] * g);
        R[i] = static_cast<float>(R[i] * g);
    }
}

void writeWav16(const std::string& path, const std::vector<float>& L, const std::vector<float>& R) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(L.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16), u16(1), u16(2), u32(44100), u32(44100 * 4), u16(4), u16(16);
    f.write("data", 4);
    u32(bytes);
    for (size_t i = 0; i < L.size(); ++i)
        for (float v : {L[i], R[i]}) u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

// --- the track -------------------------------------------------------------------------------------
double bar(double b) { return b * 4.0; }
constexpr double kEndBar = 51.0;   // the end of the outro card

// A layer's place: its clean half and its effected half (bars), for the level match.
struct Seg {
    std::string name;
    double c0, c1, f0, f1;
};

// F minor, a bar each: Fm Db Ab Eb (i VI III VII, the progression the melodic-techno tutorials teach as
// Am F C G).
const int kBassRoot[4] = {41, 37, 44, 39};   // F2 Db2 Ab2 Eb2
const int kChordRoot[4] = {53, 49, 56, 51};  // F3 Db3 Ab3 Eb3
const int kThird[4] = {3, 4, 4, 4};          // minor, then major
const int kPad[4][2] = {{56, 60}, {53, 56}, {56, 63}, {55, 58}};   // Ab3 C4, F3 Ab3, Ab3 Eb4, G3 Bb3

// The bass, a bar: off-beat sixteenths on the chord's root, the downbeats left to an imagined kick;
// 1-5-octave jumps, the accents an octave up and hit harder (FEG Velocity: they open the filter), a
// muted note on step 7, a slide from step 14. Offsets from the root; velocity 0 is a rest.
const int kBassOff[16] = {0, 0, 12, 0, 0, 0, 7, 0, 0, 0, 12, 0, 0, 7, 12, 7};
const int kBassVel[16] = {0, 96, 124, 86, 0, 96, 114, 58, 0, 96, 124, 86, 0, 100, 118, 104};
// The climax's fill, the last beat of every fourth bar: up 1-5-8-9 into the next bar's root.
const int kFillOff[4] = {0, 7, 12, 14}, kFillVel[4] = {104, 110, 118, 126};

// solo: -1 plays everything; 0..3 only that layer (the level match measures layers on their own).
void song(Session& s, std::vector<Seg>& segs, const std::vector<double>& fxGainDb, int solo) {
    Inst* master = s.addState("bus", "ef",
                              "effectforce 1\neq_on=On\neq_lc=28\neq_hf=9000\neq_hg=1.5\n"
                              "cmp_on=On\ncmp_thr=-14\ncmp_ratio=2\ncmp_att=0.03\ncmp_rel=0.25\n", 0.0);
    master->watched = true;
    s.fx = master;
    if (solo >= 0) {   // measured before the bus: its compressor would halve the level match above its threshold
        s.opt(master, 0, "eq_on", "Off");
        s.opt(master, 0, "cmp_on", "Off");
    }
    // Every chain but the bass's cuts below 120-180 Hz; delays are generous, reverbs (but the pad's)
    // moderate; the pump (a gate closed on each beat's first sixteenth) stands in for a sidechain.
    struct Layer { const char* name; const char* sf; const char* fx; double gainDb; };
    const Layer layers[] = {
        {"bass", "02_Bass/16_Grit_Roller",
         "effectforce 1\ncmp_on=On\ncmp_thr=-16\ncmp_ratio=3\ncmp_att=0.01\ncmp_rel=0.12\n"
         "dly_on=On\ndly_mode=Ping-Pong\ndly_div=1/8.\ndly_fb=0.25\ndly_hc=3000\ndly_mix=0.12\n"
         "rev_on=On\nrev_mode=Plate\nrev_decay=1.2\nrev_mix=0.1\n", 0.0},
        {"arp", "04_Keys/01_Pluck",
         "effectforce 1\neq_on=On\neq_lc=150\nchr_on=On\nchr_mode=Dimension\nchr_mix=0.35\n"
         "pls_on=On\npls_mode=Gate\npls_div=1/16\npls_pattern=Pump\npls_depth=0.5\npls_length=1\npls_smooth=0.03\n"
         "dly_on=On\ndly_mode=Ping-Pong\ndly_div=1/8\ndly_fb=0.45\ndly_hc=5000\ndly_mix=0.38\n"
         "rev_on=On\nrev_mode=Hall\nrev_size=0.75\nrev_decay=3.5\nrev_mod=0.4\nrev_mix=0.22\n", -3.0},
        {"lead", "03_Lead/08_Afterglow_Lead",
         "effectforce 1\neq_on=On\neq_lc=180\nchr_on=On\nchr_mode=Dimension\nchr_mix=0.3\n"
         "dly_on=On\ndly_mode=Ping-Pong\ndly_div=1/4.\ndly_fb=0.45\ndly_hc=4500\ndly_wow=0.15\ndly_mix=0.3\n"
         "rev_on=On\nrev_mode=Hall\nrev_size=0.8\nrev_decay=4\nrev_mod=0.4\nrev_mix=0.25\n", -2.0},
        {"pad", "07_Pad/01_Slow_Bloom_Duo",
         "effectforce 1\neq_on=On\neq_lc=120\nchr_on=On\nchr_mode=Ensemble\nchr_mix=0.5\n"
         "pls_on=On\npls_mode=Gate\npls_div=1/16\npls_pattern=Pump\npls_depth=0.6\npls_length=1\npls_smooth=0.03\n"
         "rev_on=On\nrev_mode=Hall\nrev_size=0.85\nrev_decay=7\nrev_mod=0.5\nrev_shim=0.35\nrev_mix=0.5\nout_gain=5\n", -5.0},
    };
    std::vector<Inst*> L;
    for (int k = 0; k < 4; ++k) {
        Inst* in = s.add(layers[k].name, "sf", std::string("SubForce/presets/Factory/") + layers[k].sf + ".sfp", layers[k].gainDb);
        in->watched = true;
        s.addInsertState(in, std::string("fx_") + layers[k].name, layers[k].fx);
        in->fxGain = std::pow(10.0, (k < static_cast<int>(fxGainDb.size()) ? fxGainDb[k] : 0.0) / 20.0);
        L.push_back(in);
    }
    Inst *bass = L[0], *arp = L[1], *lead = L[2], *pad = L[3];
    // The build's effects, SubForce too: a riser into the drop, an impact on it.
    Inst* riser = s.add("riser", "sf", "SubForce/presets/Factory/05_FX/05_Riser_Engine.sfp", -7.0);
    riser->watched = true;
    s.addInsertState(riser, "fx_riser", "effectforce 1\neq_on=On\neq_lc=200\ndly_on=On\ndly_mode=Ping-Pong\ndly_div=1/8.\n"
                                        "dly_fb=0.5\ndly_mix=0.3\nrev_on=On\nrev_mode=Hall\nrev_decay=6\nrev_mix=0.45\n");
    Inst* boom = s.add("boom", "sf", "SubForce/presets/Factory/05_FX/07_Impact_Boom.sfp", -3.0);
    s.addInsertState(boom, "fx_boom", "effectforce 1\nrev_on=On\nrev_mode=Hall\nrev_size=0.9\nrev_decay=5\nrev_mix=0.3\n");
    auto plays = [&](Inst* in) { return solo < 0 || in == L[static_cast<size_t>(solo)]; };
    auto N = [&](Inst* in, double beat, double len, int key, int vel) {
        if (plays(in)) s.note(in, beat, len, key, vel);
    };
    // A knob turned from a to b (its displayed value: Hz, %, ms...) over bars b0..b1, as a Q-Link would,
    // the hand never still: it wobbles `wob` of the knob's travel around the line, a cycle every `period`
    // bars (the tutorials: automate the cutoff all the time, subtly). Eighth-beat ramps; the wobble fades
    // in and out so the ends land on `from` and `to`.
    auto move = [&](Inst* in, double b0, double b1, const std::string& key, double from, double to, double wob, double period) {
        const float n0 = s.valueFor(in, key, from), n1 = s.valueFor(in, key, to);
        const double x0 = bar(b0), x1 = bar(b1);
        auto at = [&](double x) {
            const double t = (x - x0) / (x1 - x0);
            const double w = wob * std::min({1.0, t * 8.0, (1.0 - t) * 8.0}) * std::sin(2.0 * M_PI * (x - x0) / bar(period));
            return static_cast<float>(std::clamp(n0 + (n1 - n0) * t + w, 0.0, 1.0));
        };
        for (double x = x0; x < x1 - 1e-9; x += 0.125) {
            const double y = std::min(x + 0.125, x1);
            s.ramp(in, x, y, key, at(x), at(y));
        }
    };

    // The lead thickened, two oscillators sounding like four: osc 2 in the pulse range, its width swept
    // by bus 2's slow triangle (PWM), osc 1 between saw and square, a little detune, Beat Freq, Analog up.
    s.set(lead, 0, "o1_wave", 0.45f);
    s.set(lead, 0, "o2_wave", 0.72f);
    s.set(lead, 0, "o2_freq", s.valueFor(lead, "o2_freq", -0.1));
    s.set(lead, 0, "o2_beat", s.valueFor(lead, "o2_beat", 0.9));
    s.set(lead, 0, "drift", 0.5f);
    s.opt(lead, 0, "m2_src", "Triangle");
    s.set(lead, 0, "m2_rate", s.valueFor(lead, "m2_rate", 0.9));
    s.opt(lead, 0, "m2_dest", "Wave 2");
    s.set(lead, 0, "m2_amt", s.valueFor(lead, "m2_amt", 15));
    s.opt(lead, 0, "m2_ctl", "Always");
    s.opt(lead, 0, "glide_mode", "Legato");
    // The pluck a little wider and warmer: Beat Freq, Analog.
    s.set(arp, 0, "o2_beat", s.valueFor(arp, "o2_beat", 0.7));
    s.set(arp, 0, "drift", 0.35f);
    s.opt(bass, 0, "glide_mode", "Legato");
    s.set(bass, 0, "glide", s.valueFor(bass, "glide", 35));
    s.wet(pad, 0.0, 1.0f, 0.01);
    s.wet(riser, 0.0, 1.0f, 0.01);
    s.wet(boom, 0.0, 1.0f, 0.01);

    auto bassBars = [&](double b0, double b1, bool fills) {
        for (double b = b0; b < b1; ++b) {
            const int c = static_cast<int>(b) % 4, r = kBassRoot[c];
            for (int k = 0; k < 16; ++k) {
                const bool fill = fills && c == 3 && k >= 12;
                const int off = fill ? kFillOff[k - 12] : kBassOff[k], vel = fill ? kFillVel[k - 12] : kBassVel[k];
                if (vel) N(bass, bar(b) + k * 0.25, k == 7 ? 0.08 : (k == 14 && !fill) ? 0.3 : 0.19, r + off, vel);
            }
        }
    };
    // The pluck: a tresillo (3+3+2 sixteenths, twice), sparse on purpose: the tutorials' simple arp
    // that the delay finishes; the 1/8 ping-pong lands its echoes in the gaps.
    auto arpBars = [&](double b0, double b1) {
        static const int step[6] = {0, 3, 6, 8, 11, 14}, vel[6] = {112, 84, 100, 92, 84, 104};
        for (double b = b0; b < b1; ++b) {
            const int c = static_cast<int>(b) % 4, root = kChordRoot[c];
            const int tones[6] = {0, 7, 12 + kThird[c], 12, 7, 19};
            for (int k = 0; k < 6; ++k) N(arp, bar(b) + step[k] * 0.25, 0.2, root + tones[k], vel[k]);
        }
    };
    // The lead: long notes, overlapping (legato glide), a scoop into each phrase, vibrato on the long ones.
    struct Note { double at, len; int key; };
    auto leadLine = [&](double b0, const std::vector<Note>& mel, const std::vector<double>& scoops,
                        const std::vector<std::pair<double, double>>& vib) {
        for (const Note& n : mel) N(lead, bar(b0) + n.at, n.len + 0.06, n.key, 100);
        if (!plays(lead)) return;
        for (double at : scoops) {   // the bend from below, back in a third of a beat
            for (int k = 0; k <= 6; ++k) s.bend(lead, bar(b0) + at + k * 0.05, -0.55f * (1.0f - k / 6.0f));
        }
        for (const auto& v : vib) {
            s.cc(lead, bar(b0) + v.first, 1, 95);
            s.cc(lead, bar(b0) + v.second, 1, 0);
        }
    };
    // A delay throw at a phrase's end: the lead's echo turned up for its last beats, then back.
    auto throwAt = [&](double b) {
        if (!plays(lead)) return;
        Inst* fx = lead->insert;
        s.ramp(fx, bar(b) - 1.0, bar(b), "dly_fb", s.valueFor(fx, "dly_fb", 45), s.valueFor(fx, "dly_fb", 72));
        s.ramp(fx, bar(b) - 1.0, bar(b), "dly_mix", s.valueFor(fx, "dly_mix", 30), s.valueFor(fx, "dly_mix", 50));
        s.ramp(fx, bar(b) + 4.0, bar(b) + 6.0, "dly_fb", s.valueFor(fx, "dly_fb", 72), s.valueFor(fx, "dly_fb", 45));
        s.ramp(fx, bar(b) + 4.0, bar(b) + 6.0, "dly_mix", s.valueFor(fx, "dly_mix", 50), s.valueFor(fx, "dly_mix", 30));
    };
    const std::vector<Note> melodyA = {{0, 2, 72},   {2, 1, 68},  {3, 1, 67},   {4, 3, 65},  {7, 1, 68},  {8, 2, 75},
                                       {10, 1, 72},  {11, 1, 70}, {12, 4, 67},  {16, 1.5, 72}, {17.5, 0.5, 73},
                                       {18, 1, 72},  {19, 1, 68}, {20, 2, 65},  {22, 1, 68}, {23, 1, 72},
                                       {24, 1.5, 75}, {25.5, 0.5, 77}, {26, 1, 75}, {27, 1, 72}, {28, 3, 70},
                                       {31, 1, 67}};
    const std::vector<Note> melodyB = {{0, 2, 77},  {2, 1, 75},  {3, 1, 72},  {4, 3, 73},  {7, 1, 72},  {8, 2, 75},
                                       {10, 1, 77}, {11, 1, 79}, {12, 2, 79}, {14, 1, 77}, {15, 1, 75}, {16, 3, 77},
                                       {19, 1, 80}, {20, 2, 77}, {22, 2, 73}, {24, 2, 75}, {26, 2, 72}, {28, 2, 70},
                                       {30, 2, 67}};
    auto padBars = [&](double b0, double b1) {
        for (double b = b0; b < b1; ++b) {
            const int c = static_cast<int>(b) % 4;
            N(pad, bar(b), 3.95, kPad[c][0], 90);
            N(pad, bar(b), 3.95, kPad[c][1], 90);
        }
    };

    // ---- intro: the title over a pad chord, its filter opening ------------------------------------------
    s.section(0, "intro");
    padBars(0, 2);
    move(pad, 0, 2, "f_cut", 500, 1400, 0.0, 2);

    // ---- the bass: a filter build that never stands still ------------------------------------------------
    s.section(bar(2), "Bass sequence");
    s.cue(bar(2), "{\"type\": \"mode\", \"mode\": \"clean\"}");
    s.page(bar(2), bass, "FILTER");
    s.caption(bar(2), "Bass: Grit Roller", "Off-beat sixteenths on the roots, 1-5-octave jumps; the accents hit harder and open the filter");
    bassBars(2, 34, false);
    move(bass, 2, 10, "f_cut", 140, 700, 0.04, 2);
    move(bass, 2, 10, "f_res", 20, 40, 0.0, 2);
    s.wet(bass, bar(6) - 0.02, 1.0f, 0.02);
    s.cue(bar(6), "{\"type\": \"mode\", \"mode\": \"fx\"}");
    s.page(bar(6), bass->insert, "COMP");
    s.caption(bar(6), "+ EffectForce", "Compression, a short ping-pong and a plate: the bass stays tight");
    segs.push_back({"bass", bar(2), bar(6), bar(6), bar(10)});

    // ---- the pluck -----------------------------------------------------------------------------------------
    s.section(bar(10), "Pluck arpeggio");
    s.cue(bar(10), "{\"type\": \"mode\", \"mode\": \"clean\"}");
    s.page(bar(10), arp, "FILTER");
    s.caption(bar(10), "Pluck: a tresillo", "3 + 3 + 2 sixteenths, sparse on purpose: the delay will fill it");
    arpBars(10, 36);
    move(arp, 10, 18, "f_cut", 320, 1100, 0.03, 2);
    move(arp, 10, 18, "fe_d", 140, 320, 0.0, 2);
    s.gainAt(bass, bar(10), -4.0);   // the bass steps back while the arp comes in clean
    s.gainAt(bass, bar(14), 0.0);
    move(bass, 10, 14, "f_cut", 700, 480, 0.04, 2);
    move(bass, 14, 18, "f_cut", 480, 900, 0.04, 2);
    s.wet(arp, bar(14) - 0.02, 1.0f, 0.02);
    s.cue(bar(14), "{\"type\": \"mode\", \"mode\": \"fx\"}");
    s.page(bar(14), arp->insert, "DELAY");
    s.caption(bar(14), "+ EffectForce", "A 1/8 ping-pong lands in the gaps; chorus, a pump and a hall");
    segs.push_back({"arp", bar(10), bar(14), bar(14), bar(18)});

    // ---- the lead ------------------------------------------------------------------------------------------
    s.section(bar(18), "Lead");
    s.cue(bar(18), "{\"type\": \"mode\", \"mode\": \"clean\"}");
    s.page(bar(18), lead, "KEYS");
    s.caption(bar(18), "Lead: Afterglow Lead, thickened", "Osc 2's pulse width swept by an LFO, detune and Beat Freq: two oscillators, four voices' worth");
    leadLine(18, melodyA, {0.0, 16.0}, {{4.5, 7.0}, {12.5, 16.0}, {20.5, 22.5}, {28.5, 31.5}});
    move(lead, 18, 26, "f_cut", 1300, 1600, 0.04, 4);
    s.page(bar(20), lead, "MOD");
    for (Inst* in : {bass, arp}) {
        s.gainAt(in, bar(18), (in == bass ? 0.0 : -3.0) - 4.0);
        s.gainAt(in, bar(22), in == bass ? 0.0 : -3.0);
    }
    s.wet(lead, bar(22) - 0.02, 1.0f, 0.02);
    s.cue(bar(22), "{\"type\": \"mode\", \"mode\": \"fx\"}");
    s.page(bar(22), lead->insert, "DELAY");
    s.caption(bar(22), "+ EffectForce", "A dotted-quarter tape delay, chorus and a hall; the echo thrown up at the phrase's end");
    move(bass, 18, 26, "f_cut", 900, 1200, 0.04, 2);
    move(arp, 18, 26, "f_cut", 1100, 900, 0.03, 4);
    throwAt(26);
    segs.push_back({"lead", bar(18), bar(22), bar(22), bar(26)});

    // ---- all together ---------------------------------------------------------------------------------
    s.section(bar(26), "All together");
    s.cue(bar(26), "{\"type\": \"mode\", \"mode\": \"together\"}");
    s.page(bar(26), bass, "FILTER");
    s.caption(bar(26), "All together", "Nothing stands still: every part's cutoff keeps moving");
    padBars(26, 48);
    move(bass, 26, 34, "f_cut", 1200, 1800, 0.05, 2);
    move(bass, 26, 34, "f_drive", 75, 90, 0.0, 2);
    move(pad, 26, 34, "f_cut", 1400, 2200, 0.05, 4);
    move(arp, 26, 30, "f_cut", 900, 1300, 0.04, 2);
    move(lead, 26, 34, "f_cut", 1600, 2000, 0.04, 4);
    leadLine(26, melodyB, {0.0, 16.0}, {{4.5, 7.0}, {12.5, 14.0}, {16.5, 19.0}, {28.5, 32.0}});
    s.page(bar(30), arp, "AMP");
    s.caption(bar(30), "The pluck rings longer", "Its amp and filter decays turned up");
    move(arp, 30, 34, "ae_d", 1200, 2600, 0.0, 2);
    move(arp, 30, 34, "fe_d", 320, 600, 0.0, 2);
    throwAt(34);

    // ---- breakdown: the bass out, the arp closing, the pad's hall swelling, osc 2 sliding up a fifth --
    s.section(bar(34), "Breakdown");
    s.cue(bar(34), "{\"type\": \"mode\", \"mode\": \"together\"}");
    s.page(bar(34), lead, "OSC");
    s.caption(bar(34), "Breakdown", "The bass drops out; on the lead's long note osc 2 slides up a fifth");
    move(arp, 34, 36, "f_cut", 1300, 260, 0.0, 2);
    move(pad, 34, 38, "f_cut", 2200, 3000, 0.06, 2);
    s.gainAt(pad, bar(34), -11.0);   // stripped down: the drop has to hit harder than this
    s.gainAt(lead, bar(34), -8.0);
    s.gainAt(arp, bar(34), -7.0);
    if (plays(pad)) {
        Inst* fx = pad->insert;
        s.ramp(fx, bar(34), bar(36), "pls_depth", s.valueFor(fx, "pls_depth", 60), s.valueFor(fx, "pls_depth", 0));
        s.ramp(fx, bar(34), bar(37), "rev_mix", s.valueFor(fx, "rev_mix", 50), s.valueFor(fx, "rev_mix", 72));
        s.ramp(fx, bar(37.75), bar(38), "rev_mix", s.valueFor(fx, "rev_mix", 72), s.valueFor(fx, "rev_mix", 50));
        s.ramp(fx, bar(37.75), bar(38), "pls_depth", s.valueFor(fx, "pls_depth", 0), s.valueFor(fx, "pls_depth", 60));
    }
    // the lead: F held, then gliding (slower and slower) to Ab and C into the drop
    const std::vector<Note> rise = {{0, 8, 65}, {8, 6, 68}, {14, 2, 72}};
    leadLine(34, rise, {0.0}, {{2.0, 7.5}});
    move(lead, 34, 38, "f_cut", 2000, 2800, 0.05, 2);
    move(lead, 35, 37, "o2_freq", -0.1, 7.0, 0.0, 2);
    move(lead, 37, 38, "o2_freq", 7.0, -0.1, 0.0, 1);
    move(lead, 34, 37.5, "glide", 60, 420, 0.0, 2);
    s.page(bar(36), riser, "FILTER");
    s.caption(bar(36), "Riser: Riser Engine", "Noise and a saw-swept filter, from SubForce too, cutoff opening");
    N(riser, bar(36), bar(2) - 0.1, 53, 110);
    move(riser, 36, 38, "f_cut", 400, 7000, 0.0, 2);

    // ---- the drop: every move at once ---------------------------------------------------------------
    s.section(bar(38), "The drop");
    s.cue(bar(38), "{\"type\": \"mode\", \"mode\": \"together\"}");
    s.page(bar(38), bass, "FILTER");
    s.caption(bar(38), "The drop: Impact Boom", "Then every bass move stacked: cutoff, resonance, Multidrive, EG amount, glide");
    N(boom, bar(38), 3.0, 41, 127);
    bassBars(38, 46, true);
    move(bass, 38, 42, "f_cut", 800, 2600, 0.05, 1);
    s.gainAt(bass, bar(38), 1.5);
    s.gainAt(arp, bar(38), -2.0);
    s.gainAt(pad, bar(38), -5.0);
    s.gainAt(lead, bar(38), -2.0);
    move(bass, 42, 46, "f_cut", 2600, 1100, 0.05, 1);
    move(bass, 38, 42, "f_res", 24, 55, 0.03, 2);
    move(bass, 42, 46, "f_res", 55, 35, 0.0, 2);
    move(bass, 38, 42, "f_drive", 75, 100, 0.0, 2);
    move(bass, 38, 42, "f_env", 2.8, 4.5, 0.0, 2);   // octaves
    move(bass, 38, 44, "glide", 35, 90, 0.0, 2);
    arpBars(38, 46);
    move(arp, 38, 46, "f_cut", 600, 1500, 0.05, 2);
    move(lead, 38, 46, "f_cut", 1600, 2400, 0.05, 2);
    move(lead, 38, 38.25, "glide", 420, 60, 0.0, 1);
    s.opt(lead, bar(38) - 0.05, "o2_oct", "4'");
    leadLine(38, melodyB, {0.0, 16.0}, {{4.5, 7.0}, {12.5, 14.0}, {16.5, 19.0}, {28.5, 32.0}});
    move(pad, 38, 46, "f_cut", 1800, 2600, 0.05, 4);
    s.page(bar(42), master, "COMP");
    s.caption(bar(42), "EffectForce on the master", "EQ and glue over six SubForce parts, each through its own EffectForce");

    // ---- the end: the filters close, the halls ring out -------------------------------------------------
    s.section(bar(46), "Ending");
    s.cue(bar(46), "{\"type\": \"mode\", \"mode\": \"together\"}");
    s.page(bar(46), pad, "FILTER");
    s.caption(bar(46), "Ending", "The filters close, the halls ring out");
    move(pad, 46, 48, "f_cut", 2600, 300, 0.0, 2);
    move(arp, 46, 48, "f_cut", 1500, 200, 0.0, 2);
    move(pad, 46, 48.5, "volume", -1.2, -18, 0.0, 2);
    move(lead, 46, 48.5, "volume", -9.2, -26, 0.0, 2);
    move(arp, 46, 48, "volume", 4.8, -10, 0.0, 2);
    N(lead, bar(46), bar(2) - 0.2, 65, 100);
    if (plays(lead)) {
        s.cc(lead, bar(46) + 1.0, 1, 100);
        s.cc(lead, bar(48), 1, 0);
    }
    arpBars(46, 48);
    s.section(bar(48), "outro");
}

double lufsOf(const std::vector<float>& L, const std::vector<float>& R, double b0, double b1) {
    const size_t a = static_cast<size_t>(b0 * kSpb), e = std::min(L.size(), static_cast<size_t>(b1 * kSpb));
    if (e <= a) return -100.0;
    return integratedLufs(std::vector<float>(L.begin() + static_cast<long>(a), L.begin() + static_cast<long>(e)),
                          std::vector<float>(R.begin() + static_cast<long>(a), R.begin() + static_cast<long>(e)));
}

}  // namespace

int main(int argc, char** argv) {
    const bool snapshot = argc == 5 && !std::strcmp(argv[1], "--snapshot");
    if (argc != 2 && !snapshot) {
        std::fprintf(stderr, "usage: %s <out dir> | --snapshot <pf|sf|ef> <preset under D:/DEV> <out.jsonl>\n", argv[0]);
        return 2;
    }
    char tmpl[] = "/tmp/showreel.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    for (const char* v : {"PF_DATA_DIR", "PF_TABLE_ROOTS", "PF_PRESET_ROOTS", "PF_TUNING_ROOTS", "SF_DATA_DIR", "SF_PRESET_ROOTS",
                          "EF_DATA_DIR", "EF_PRESET_ROOTS"})
        setenv(v, dir, 1);
    setenv("PF_CPU_GUARD", "0", 1);
    setenv("SF_FIXED_SEED", "1", 1);
    setenv("EF_FIXED_SEED", "1", 1);

    Session s;
    s.libs.push_back(loadLib("pf", kDev + "PolyForce/build/polyforce.so", kDev + "PolyForce/surface/params.json"));
    s.libs.push_back(loadLib("sf", kDev + "SubForce/build/subforce.so", kDev + "SubForce/surface/params.json"));
    s.libs.push_back(loadLib("ef", kDev + "EffectForce/build/effectforce.so", kDev + "EffectForce/surface/params.json"));
    if (snapshot) {   // one instance's state, a beat after its preset loads
        s.add("snap", argv[2], argv[3], 0.0)->watched = true;
        std::ofstream state(argv[4]);
        std::vector<float> L, R;
        s.run(1.0, state, L, R);
        return 0;
    }
    const std::string out = argv[1];
    // Pass 1, a layer at a time: each layer's clean and effected halves on their own, measured.
    std::vector<double> fxGainDb(4, 0.0);
    for (int solo = 0; solo < 3; ++solo) {
        Session m;
        m.libs.push_back(loadLib("pf", kDev + "PolyForce/build/polyforce.so", kDev + "PolyForce/surface/params.json"));
        m.libs.push_back(loadLib("sf", kDev + "SubForce/build/subforce.so", kDev + "SubForce/surface/params.json"));
        m.libs.push_back(loadLib("ef", kDev + "EffectForce/build/effectforce.so", kDev + "EffectForce/surface/params.json"));
        std::vector<Seg> segs;
        song(m, segs, {}, solo);
        std::ofstream none("/dev/null");
        std::vector<float> L, R;
        m.run(bar(kEndBar), none, L, R);
        const Seg& g = segs[static_cast<size_t>(solo)];
        const double c = lufsOf(L, R, g.c0, g.c1), f = lufsOf(L, R, g.f0, g.f1);
        fxGainDb[static_cast<size_t>(solo)] = std::clamp(c - f, -9.0, 6.0);
        std::printf("%-6s clean %6.1f LUFS, effected %6.1f LUFS: %+5.1f dB\n", g.name.c_str(), c, f, fxGainDb[static_cast<size_t>(solo)]);
    }
    std::vector<Seg> segs;
    song(s, segs, fxGainDb, -1);

    std::ofstream state(out + "/state.jsonl");
    std::vector<float> L, R;
    const double endBeat = bar(kEndBar);
    s.run(endBeat, state, L, R);

    std::ofstream cues(out + "/cues.jsonl");
    char head[160];
    std::snprintf(head, sizeof head, "{\"t\": 0, \"type\": \"song\", \"bpm\": %.1f, \"fps\": %.0f, \"seconds\": %.4f}", kBpm, kFps,
                  static_cast<double>(L.size()) / kSr);
    cues << head << "\n";
    for (const std::string& c : s.cues) cues << c << "\n";

    std::printf("%-6s %8s %8s\n", "inst", "RMS dB", "peak dB");
    for (auto& in : s.insts)
        if (in->loudBlocks)
            std::printf("%-6s %8.1f %8.1f\n", in->name.c_str(), 10.0 * std::log10(in->sumSq / static_cast<double>(in->loudBlocks)),
                        20.0 * std::log10(std::max(in->peak, 1e-9)));
    double pk = 0.0;
    for (size_t i = 0; i < L.size(); ++i) pk = std::max(pk, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
    const double lufs = integratedLufs(L, R);
    std::printf("raw mix: %.1f LUFS, peak %.1f dBFS\n", lufs, 20.0 * std::log10(std::max(pk, 1e-9)));
    const double g = std::pow(10.0, (-10.0 - lufs) / 20.0);
    for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(L[i] * g), R[i] = static_cast<float>(R[i] * g);
    limit(L, R, std::pow(10.0, -1.0 / 20.0));
    std::printf("mastered: %.1f LUFS\n", integratedLufs(L, R));
    writeWav16(out + "/mix.wav", L, R);
    return 0;
}
