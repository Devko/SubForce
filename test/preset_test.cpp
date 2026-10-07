// Saved state, presets and the surface: chunk round trips, the preset stepper and buttons, user
// presets, the browser, favorites, randomize, stepping, pushing values back to MPC, and every
// factory preset playing.
#include "host.h"
#include "../plugin/presets.h"
#include "factory_presets.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace sft {
namespace {

void testState() {
    std::printf("== saved state\n");
    Host a;
    a.set(sf::P_F_CUT, 333.0f);
    a.set(sf::P_O1_OCT, 1);
    a.set(sf::P_F_SLOPE, sf::SL_12);
    a.set(sf::P_M2_DEST, sf::MD_VOLUME);
    a.set(sf::P_AE_R, 1.5f);
    const std::string s = a.chunk();
    CHECK(s.compare(0, 11, "subforce 1\n") == 0);
    CHECK(s.find("f_cut=333\n") != std::string::npos && s.find("ae_r=1.5\n") != std::string::npos);
    Host b;
    CHECK(b.load(s) == 1);
    for (int i = 0; i < sf::P_COUNT; ++i)
        if (sf::PARAM_INFO[i].kind == sf::Kind::Synth && std::fabs(a.get(i) - b.get(i)) > 1e-5f) {
            std::printf("  %s differs after a round trip\n", sf::PARAM_INFO[i].key);
            CHECK(false);
        }
    // A project's state changes only what it lists; unknown keys and bad numbers are skipped.
    Host c;
    c.set(sf::P_F_RES, 0.5f);
    c.set(sf::P_F_DRIVE, 0.7f);
    c.set(sf::P_F_KB, 1.5f);
    CHECK(c.load("subforce 1\nf_cut=100\nnot_a_param=3\nf_drive=abc\nf_kb=1,5\n") == 1);
    CHECK(std::fabs(c.value(sf::P_F_CUT) - 100.0f) < 0.1f && std::fabs(c.value(sf::P_F_RES) - 0.5f) < 1e-4f);
    CHECK(std::fabs(c.value(sf::P_F_DRIVE) - 0.7f) < 1e-4f && std::fabs(c.value(sf::P_F_KB) - 1.5f) < 1e-4f);   // left as they were
    // Out of range values clamp; BOM and CRLF are fine; other text is refused.
    CHECK(c.load("\xEF\xBB\xBFsubforce 1\r\nf_cut=99999\r\n") == 1 && std::fabs(c.value(sf::P_F_CUT) - 20000.0f) < 1.0f);
    CHECK(c.load("polyforce 4\nvolume=0\n") == 0 && c.load("") == 0 && c.load("subforce x\n") == 0);
    CHECK(c.e->dispatcher(c.e, vst::effSetChunk, 0, 0, nullptr, 0.0f) == 0);
    // The loaded patch is what plays (a 2' octave a project asked for).
    Host d;
    d.bare();
    CHECK(d.load("subforce 1\no1_oct=4\nmix_o1=0.8\nf_cut=20000\nf_env=0\nf_kb=0\ndrift=0\n") == 1);
    d.on(45);
    d.run(kBlocksPerSec / 4);
    d.run(kBlocksPerSec / 2);
    CHECK(std::fabs(pitchHz(d.L) - 440.0) < 0.6);
}

void testPresets() {
    std::printf("== presets\n");
    const std::string root = fixtureDir() + "/presets";
    Host h;
    // INIT loads the Init preset (the defaults, at its matched volume).
    h.set(sf::P_F_CUT, 50.0f);
    h.press(sf::P_PRE_INIT);
    CHECK(std::fabs(h.get(sf::P_F_CUT) - sf::PARAM_INFO[sf::P_F_CUT].def) < 1e-5f);
    CHECK(h.display(sf::P_PRESET) == "PRESET  Templates / Init");
    // SAVE writes User 001.sfp, then User 002; the stepper shows it.
    h.set(sf::P_F_CUT, 444.0f);
    h.press(sf::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 001.sfp"));
    CHECK(h.display(sf::P_PRESET) == "PRESET  User / User 001");
    std::string text;
    CHECK(sf::presetText("plugin:User/User 001.sfp", text) && text.find("f_cut=444\n") != std::string::npos &&
          text.find("preset=") == std::string::npos && text.find("unit=") == std::string::npos);   // a sound, not an instrument
    h.press(sf::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 002.sfp"));
    // Next / previous walk the flat list; the stepper turns one preset per event.
    h.press(sf::P_PRE_INIT);
    const auto L = sf::presetLibrary().listing();
    const int at = L->find("builtin:Init");
    CHECK(at >= 0);
    h.press(sf::P_PRESET_NEXT);
    CHECK(h.display(sf::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    h.press(sf::P_PRESET_PREV);
    CHECK(h.display(sf::P_PRESET) == "PRESET  Templates / Init");
    const float n = h.get(sf::P_PRESET);
    h.setN(sf::P_PRESET, n + 1.0f / 128.0f);   // one Q-Link detent
    CHECK(h.display(sf::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 1)].key));
    // A project remembers its preset; a preset file doesn't name one.
    CHECK(h.chunk().find("\npreset=" + L->items[static_cast<size_t>(at + 1)].key + "\n") != std::string::npos);
    // A preset that went missing is ignored: NEXT steps on from where the stepper stood.
    const std::string here = h.display(sf::P_PRESET);
    h.load("subforce 1\npreset=plugin:User/Gone.sfp\n");
    CHECK(h.display(sf::P_PRESET) == "PRESET  Gone");
    h.press(sf::P_PRESET_NEXT);
    h.run(4);
    CHECK(h.finite && h.display(sf::P_PRESET) != "PRESET  Gone" && h.display(sf::P_PRESET) != here);

    // PREV at the first preset, NEXT at the last: nothing to load, the edits stay.
    CHECK(at == 0);   // Init is the first preset of all
    h.press(sf::P_PRE_INIT);
    h.set(sf::P_F_CUT, 300.0f);
    h.press(sf::P_PRESET_PREV);
    CHECK(std::fabs(h.value(sf::P_F_CUT) - 300.0f) < 0.5f);
    const std::string lastKey = L->items.back().key;
    h.load("subforce 1\npreset=" + lastKey + "\n");
    h.set(sf::P_F_CUT, 300.0f);
    h.press(sf::P_PRESET_NEXT);
    CHECK(std::fabs(h.value(sf::P_F_CUT) - 300.0f) < 0.5f && h.display(sf::P_PRESET) == "PRESET  " + L->label(lastKey));
    // RANDOM, then NEXT: the preset after the one the sound came from, not the first of all.
    h.press(sf::P_PRE_INIT);
    h.press(sf::P_PRESET_NEXT);
    h.press(sf::P_PRESET_NEXT);   // the third preset
    const std::string third = h.display(sf::P_PRESET);
    h.press(sf::P_PRE_RAND);
    CHECK(h.display(sf::P_PRESET) == "PRESET  -");
    h.press(sf::P_PRESET_NEXT);
    CHECK(h.display(sf::P_PRESET) == "PRESET  " + L->label(L->items[static_cast<size_t>(at + 3)].key) && third != h.display(sf::P_PRESET));

    // User numbers are never reused, even after the newest file is deleted.
    std::filesystem::remove(root + "/User/User 002.sfp");
    h.press(sf::P_PRE_SAVE);
    CHECK(std::filesystem::exists(root + "/User/User 003.sfp") && !std::filesystem::exists(root + "/User/User 002.sfp"));

    // Rand Amount is the surface's: not saved, not reset by a preset.
    h.set(sf::P_RAND_AMT, 1.0f);
    CHECK(h.chunk().find("rand_amt") == std::string::npos);
    h.press(sf::P_PRE_INIT);
    CHECK(h.value(sf::P_RAND_AMT) == 1.0f);

    // A preset file added while MPC runs shows up in a new instance, and when browsing.
    std::filesystem::create_directories(root + "/Pads");
    std::ofstream(root + "/Pads/Warm.sfp") << "subforce 1\nf_cut=900\n";
    Host fresh;
    CHECK(sf::presetLibrary().listing()->find("plugin:Pads/Warm.sfp") >= 0);
    std::ofstream(root + "/Pads/Cold.sfp") << "subforce 1\nf_cut=300\n";
    fresh.setN(sf::P_CAT_1, 1.0f);   // a category tap looks at the folders again
    CHECK(sf::presetLibrary().listing()->find("plugin:Pads/Cold.sfp") >= 0);
    // A file renamed under the listing: the stepper moves past it instead of sticking.
    fresh.press(sf::P_PRE_INIT);
    std::filesystem::rename(root + "/Pads/Cold.sfp", root + "/Pads/Cool.sfp");
    for (int k = 0; k < sf::kNumFactoryPresets + 8; ++k) fresh.press(sf::P_PRESET_NEXT);
    CHECK(fresh.display(sf::P_PRESET) != "PRESET  " + L->label("plugin:Pads/Cold.sfp"));
}

void testBrowser() {
    std::printf("== preset browser\n");
    Host h;
    h.press(sf::P_PRE_INIT);
    h.run(8);
    // Categories: FAVORITES, RECENT, then the factory folders, then the user's.
    CHECK(h.display(sf::P_CAT_1) == "FAVORITES" && h.display(sf::P_CAT_2) == "RECENT" && h.display(sf::P_CAT_3) == "TEMPLATES");
    CHECK(h.get(sf::P_CAT_3) > 0.5f);   // Init's category is lit
    CHECK(h.display(sf::P_ITEM_1) == "Init" && h.get(sf::P_ITEM_1) > 0.5f);
    CHECK(h.display(sf::P_ITEM_PAGE) == "PAGE 1 / 1");
    // A tap on a preset tile loads it.
    int tile = -1;
    for (int t = 0; t < sf::kBrowserItems; ++t)
        if (!h.display(sf::P_ITEM_1 + t).empty() && h.display(sf::P_ITEM_1 + t) != "Init") tile = sf::P_ITEM_1 + t;
    if (tile >= 0) {
        const std::string name = h.display(tile);
        h.setN(tile, 1.0f);
        CHECK(h.display(sf::P_BR_NOW) == "PRESET  Templates / " + name);
    }
    // Favorite: toggled, kept in the data folder, listed under FAVORITES.
    h.setN(sf::P_FAV, 1.0f);
    CHECK(h.get(sf::P_FAV) > 0.5f);
    CHECK(std::filesystem::exists(fixtureDir() + "/data/preset_favorites.txt"));
    h.setN(sf::P_CAT_1, 1.0f);
    CHECK(h.get(sf::P_CAT_1) > 0.5f && !h.display(sf::P_ITEM_1).empty());
    // Random pick: another preset of the category.
    h.setN(sf::P_CAT_3, 1.0f);
    const std::string before = h.display(sf::P_BR_NOW);
    h.press(sf::P_RND);
    CHECK(h.display(sf::P_BR_NOW) != before);   // Templates has four presets
    // The plugin pushes the tiles it lit to MPC (audioMasterAutomate) from the audio thread.
    h.log.automated.clear();
    h.run(8);
    CHECK(!h.log.automated.empty());
}

void testStepping() {
    std::printf("== stepping (Q-Link, data wheel, taps)\n");
    Host h;
    // An enum moves one option per Q-Link detent, whatever the size of the detent.
    h.set(sf::P_F_SLOPE, sf::SL_24);
    h.setN(sf::P_F_SLOPE, h.get(sf::P_F_SLOPE) - 1.0f / 128.0f);
    CHECK(h.value(sf::P_F_SLOPE) == sf::SL_18);
    // A tap on an option (its exact value) lands on it.
    h.setN(sf::P_F_SLOPE, 0.0f);
    CHECK(h.value(sf::P_F_SLOPE) == sf::SL_6);
    // The snapped value goes back to MPC.
    h.setN(sf::P_O1_OCT, 0.5f + 0.01f);   // a wheel click off an option: one step up, to 4'
    h.log.automated.clear();
    h.run(4);
    CHECK(h.log.automated.count(sf::P_O1_OCT) == 1 && h.log.automated[sf::P_O1_OCT] == 0.75f);
    // A popup's list closes when an option is picked.
    h.setN(sf::P_M1_SRC__OPEN, 1.0f);
    CHECK(h.get(sf::P_M1_SRC__OPEN) > 0.5f);
    h.setN(sf::P_M1_SRC, 2.0f / static_cast<float>(sf::MS_COUNT - 1));
    CHECK(h.get(sf::P_M1_SRC__OPEN) == 0.0f && h.value(sf::P_M1_SRC) == sf::MS_SAW);
    // Continuous knobs follow MPC as they are.
    h.setN(sf::P_F_RES, 0.37f);
    CHECK(h.get(sf::P_F_RES) == 0.37f);
    // A Q-Link turn on the preset stepper, as the Force sends it (the read-back plus 1/128 per
    // detent, a few ms apart): a preset per detent the whole turn long, as many as NEXT taps. (It
    // used to measure each detent from MPC's previous value, and stalled after one preset.)
    {
        Host a;
        a.press(sf::P_PRE_INIT);
        const auto L = sf::presetLibrary().listing();
        const int at = L->find("builtin:Init");
        auto label = [&L](int k) { return "PRESET  " + L->label(L->items[static_cast<size_t>(k)].key); };
        CHECK(at >= 0 && at + 6 < static_cast<int>(L->items.size()));
        {
            Turn turn;
            for (int k = 0; k < 6; ++k) a.detent(sf::P_PRESET, +1);
        }
        CHECK(a.display(sf::P_PRESET) == label(at + 6));
        {
            Turn turn;
            for (int k = 0; k < 2; ++k) a.detent(sf::P_PRESET, -1);
        }
        CHECK(a.display(sf::P_PRESET) == label(at + 4));
    }
    // A tile tap's release echo (~0.7 s later) is not a second tap; a tap a second later is.
    {
        Host t;
        t.press(sf::P_PRE_INIT);   // a preset to favorite
        t.setN(sf::P_FAV, 1.0f);
        CHECK(t.get(sf::P_FAV) > 0.5f);
        {
            Turn echo(700);
            t.setN(sf::P_FAV, 0.0f);
        }
        CHECK(t.get(sf::P_FAV) > 0.5f);
        t.setN(sf::P_FAV, 0.0f);
        CHECK(t.get(sf::P_FAV) < 0.5f);
    }
}

void testRandomize() {
    std::printf("== randomize\n");
    Host h;
    h.press(sf::P_PRE_INIT);
    const float vol = h.get(sf::P_VOLUME), glide = h.get(sf::P_GLIDE);
    h.set(sf::P_RAND_AMT, 1.0f);
    h.press(sf::P_PRE_RAND);
    CHECK(h.get(sf::P_VOLUME) == vol && h.get(sf::P_GLIDE) == glide);   // left alone
    CHECK(h.display(sf::P_PRESET) == "PRESET  -");
    bool changed = false;
    for (int i : {sf::P_F_CUT, sf::P_O1_WAVE, sf::P_AE_D, sf::P_MIX_O2}) changed = changed || h.get(i) != sf::PARAM_INFO[i].def;
    CHECK(changed);
    for (int k = 0; k < 10; ++k) {
        h.press(sf::P_PRE_RAND);
        h.on(40 + k);
        const float peak = h.run(40);
        CHECK(peak > 1e-3f && peak < 2.0f);
        h.off(40 + k);
    }
    CHECK(h.finite);
}

void testFactory() {
    std::printf("== factory presets\n");
    CHECK(sf::kNumFactoryPresets >= 1);
    int quiet = 0, loud = 0;
    for (int i = 0; i < sf::kNumFactoryPresets; ++i) {
        Host h;
        const std::string key = std::string("builtin:") + sf::kFactoryPresets[i].name;
        std::string text;
        CHECK(sf::presetText(key, text));
        CHECK(h.load(text) == 1);   // the stepper / browser path is tested above; here, the sound
        h.on(36 + (i * 7) % 24, 100);
        float peak = h.run(kBlocksPerSec);
        h.off(36 + (i * 7) % 24);
        peak = std::max(peak, h.run(kBlocksPerSec / 2));
        if (peak < 0.01f) {
            ++quiet;
            std::printf("  %s: peak %.4f\n", sf::kFactoryPresets[i].name, peak);
        }
        if (peak > 1.0f) {
            ++loud;
            std::printf("  %s: peak %.2f\n", sf::kFactoryPresets[i].name, peak);
        }
        CHECK(h.finite);
    }
    CHECK(quiet == 0 && loud == 0);
}

} // namespace

void presetTests() {
    testState();
    testPresets();
    testBrowser();
    testStepping();
    testRandomize();
    testFactory();
}

} // namespace sft
