#!/usr/bin/env python3
"""SubForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json        ordered VST parameter list (index = position; append-only once released:
                     MPC projects store values by index)
  layout.conf        the skin (shadow_page.conf syntax, see
                     third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                     Force-Shadow pixels, the plugin area is y = 86..714
  vst.json           plugin identity for the vendored gen_vst.py
  build/param_ids.h  everything the C++ is compiled against: parameter ids, kinds, value
                     curves, names, options and defaults (the C++ never reads gen_vst's params.h,
                     so `make test` and the .so build need only Python, not the skin toolchain)
  build/factory_presets.h  the factory presets (presets/Factory/*/*.sfp), checked and embedded
  build/skin_style.json  the palette, knob looks and primary buttons for skin_polish.py, which
                     `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry with shadow_skin's own sizes (inside the plugin
area, no overlaps within a page, nothing in a card's title band, open popup lists inside the
plugin area, bitmap-font glyphs) and the parameter names MPC shows (short, unique), so a broken
page fails here instead of on the device. The layout machinery is PolyForce's, unchanged.

Run: python3 surface/surface.py   (make surface does this)
"""
import json
import math
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STEPPER_RANGE = 1023        # a list stepper's VST range: 0..1023 items (stepItem moves 1 per event)
BROWSER_CATS = 16           # category tiles on the browser page (2 x 8)
BROWSER_ITEMS = 24          # preset tiles (3 x 8)

VST = {"name": "SubForce", "vendor": "Devko", "uid": "SbFc", "version": 1000,
       "so": "subforce.so", "params": "params.json", "layout": "layout.conf"}


# --- parameters ------------------------------------------------------------------------------
# kind:
#   synth    a sound parameter: saved in the state, automatable; curve lin|log|int|pow|enum
#   ui       a value the surface keeps for itself (Rand Amount): not saved, not automatable
#   readout  text the plugin writes (status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (presets), text = the item; moves one item per
#            Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a browser tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
#   meter    a value the plugin sets for a display-only filmstrip: not saved, not automatable
# fmt: how the plugin prints the value (see plugin/patch_map.cpp paramDisplay)
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def meter_param(key, name):
    _add(key, name, "meter", "lin", 0, 1, 0.5, "none")


def enum(key, name, options, default, ui=False):
    _add(key, name, "ui" if ui else "synth", "enum", 0, len(options) - 1, options.index(default), "enum",
         options=options)


def num(key, name, curve, lo, hi, default, fmt, ui=False):
    _add(key, name, "ui" if ui else "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " Prev")
    button(key + "_next", name + " Next")


def button(key, name):
    _add(key, name, "button", "int", 0, 1, 0, "none")


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)




readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("volume", "Volume", "lin", -60, 6, -4.5, "db")

# --- oscillators (dsp/osc.h) ---
OCTAVES = ["32'", "16'", "8'", "4'", "2'"]          # dsp/synth.h kOctaveMin..kOctaveMax
ON_OFF = ["Off", "On"]
enum("o1_oct", "Osc 1 Octave", OCTAVES, "8'")
num("o1_wave", "Osc 1 Wave", "lin", 0, 1, round(1 / 3, 6), "wave")
enum("o2_oct", "Osc 2 Octave", OCTAVES, "8'")
num("o2_freq", "Osc 2 Freq", "lin", -7, 7, 0, "semifine")
num("o2_wave", "Osc 2 Wave", "lin", 0, 1, round(1 / 3, 6), "wave")
enum("o2_sync", "Hard Sync", ON_OFF, "Off")
enum("sub_oct", "Sub Octave", ["-1 Oct", "-2 Oct"], "-1 Oct")   # dsp/synth.h SubOctave
enum("kb_reset", "KB Reset", ON_OFF, "Off")
num("drift", "Analog", "lin", 0, 1, 0.25, "pct")   # drift, jitter, per-note and per-unit variation, shapes

# --- mixer ---
num("mix_o1", "Osc 1 Level", "lin", 0, 1, 0.8, "pct")
num("mix_sub", "Sub Level", "lin", 0, 1, 0, "pct")
num("mix_o2", "Osc 2 Level", "lin", 0, 1, 0, "pct")
num("mix_noise", "Noise Level", "lin", 0, 1, 0, "pct")
num("mix_fb", "Feedback", "lin", 0, 1, 0, "pct")
num("noise_color", "Noise Colour", "lin", 0, 1, 0.5, "noise")   # 0 white, 0.5 pink (the original's), 1 dark

# --- filter (dsp/ladder.h) ---
SLOPES = ["6 dB", "12 dB", "18 dB", "24 dB"]       # dsp/synth.h Slope
num("f_cut", "Cutoff", "log", 20, 20000, 2000, "hz")
num("f_res", "Resonance", "lin", 0, 1, 0, "pct")
num("f_drive", "Multidrive", "lin", 0, 1, 0.2, "pct")
enum("f_slope", "Slope", SLOPES, "24 dB")
num("f_env", "EG Amount", "lin", -1, 1, 0.3, "envamt")
num("f_kb", "Key Track", "lin", 0, 2, 0.5, "pct")

# --- envelopes (dsp/env.h): filter EG, amp EG; the same keys after the prefix ---
for e, name, (a, d, s, r, vel) in (("fe", "FEG", (0.002, 0.4, 0.3, 0.3, 0.3)),
                                   ("ae", "AEG", (0.002, 0.5, 0.8, 0.15, 0.4))):
    num(e + "_dly", name + " Delay", "pow", 0, 10, 0, "time")
    num(e + "_a", name + " Attack", "log", 0.001, 10, a, "time")
    num(e + "_hold", name + " Hold", "pow", 0, 10, 0, "time")
    num(e + "_d", name + " Decay", "log", 0.001, 10, d, "time")
    num(e + "_s", name + " Sustain", "lin", 0, 1, s, "pct")
    num(e + "_r", name + " Release", "log", 0.001, 10, r, "time")
    num(e + "_vel", name + " Velocity", "lin", 0, 1, vel, "pct")
    num(e + "_kb", name + " Key Track", "lin", 0, 1, 0, "pct")
    enum(e + "_loop", name + " Loop", ["Off", "Loop"], "Off")
    enum(e + "_reset", name + " Reset", ["Off", "Reset"], "Off")

# --- the two mod busses (dsp/mod.h) ---
# Saved state keeps an option's index: new options go at the end.
MOD_SOURCES = ["Triangle", "Square", "Saw", "Ramp", "S&H", "Smooth", "Filter EG",
               "Sine", "Noise", "Amp EG", "Velocity", "Aftertouch", "Key", "Constant"]   # ModSource
MOD_DESTS = ["Off", "Wave 1+2", "Wave 1", "Wave 2", "Resonance", "Multidrive", "Sub Level", "Noise Level",
             "Feedback", "Volume", "Other Rate", "EG Amount", "Key Track", "Osc 1 Level", "Osc 2 Level",
             "Beat Freq", "EG Time", "FEG Time", "AEG Time", "Glide Time"]          # ModDest
MOD_CONTROLS = ["Always", "Mod Wheel", "Aftertouch", "Velocity", "None"]            # ModControl
RATE_MODES = ["Free", "Sync", "Hi"]                                                 # dsp/mod.h RateMode
OSC_DESTS = ["Osc 1+2", "Osc 1", "Osc 2"]                                           # dsp/synth.h OscDest
SYNC_DIVS = ["8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/2T", "1/4", "1/4T", "1/4.", "1/8", "1/8T", "1/8.",
             "1/16", "1/16T", "1/16.", "1/32", "1/32T"]                              # dsp/mod.h kSyncBeats
for b, (rate, ctl) in ((1, (5.0, "Mod Wheel")), (2, (0.5, "Always"))):
    p = "m%d_" % b
    enum(p + "src", "M%d Source" % b, MOD_SOURCES, "Triangle")
    popup_flag(p + "src")
    enum(p + "sync", "M%d Rate Mode" % b, RATE_MODES, "Free")
    num(p + "rate", "M%d Rate" % b, "log", 0.05, 100, rate, "lfohz")
    enum(p + "div", "M%d Sync Rate" % b, SYNC_DIVS, "1/8")
    popup_flag(p + "div")
    num(p + "pitch", "M%d Pitch" % b, "lin", -1, 1, 0, "modpitch")
    enum(p + "pdest", "M%d Pitch Dest" % b, OSC_DESTS, "Osc 1+2")
    num(p + "filter", "M%d Filter" % b, "lin", -1, 1, 0, "modcut")
    enum(p + "dest", "M%d Dest" % b, MOD_DESTS, "Off")
    popup_flag(p + "dest")
    num(p + "amt", "M%d Amount" % b, "lin", -1, 1, 0, "bipct")
    enum(p + "ctl", "M%d Control" % b, MOD_CONTROLS, ctl)
    popup_flag(p + "ctl")
    enum(p + "trig", "M%d Trigger" % b, ["Free", "Retrig"], "Free")

# --- keyboard and glide ---
enum("kmode", "Key Mode", ["Mono", "Duo"], "Mono")                  # KeyMode
enum("prio", "Note Priority", ["Last", "Low", "High"], "Last")     # Priority
enum("trig", "Trigger", ["Multi", "Single"], "Multi")              # Trigger
# GlideMode, then the same gated (the original's GATED: glides move only while a key is held)
enum("glide_mode", "Glide", ["Off", "Always", "Legato", "Gated", "Legato Gated"], "Off")
enum("glide_type", "Glide Type", ["Rate", "Time", "Exp"], "Time")  # GlideType
num("glide", "Glide Time", "log", 0.001, 10, 0.08, "time")
enum("glide_dest", "Glide Dest", OSC_DESTS, "Osc 1+2")
num("bend_up", "Bend Up", "int", 0, 24, 2, "range")
num("bend_dn", "Bend Down", "int", 0, 24, 2, "range")

# --- presets ---
stepper("preset", "Preset")
button("pre_save", "Save Preset")
button("pre_init", "Init Patch")
button("pre_rand", "Randomize")
num("rand_amt", "Rand Amount", "lin", 0, 1, 0.5, "pct", ui=True)   # how far RANDOM goes: not part of a sound

# --- the preset browser ---
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev")
button("cat_next", "Categories Next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("item_%d" % i, "Item %d" % i)
button("item_prev", "Items Prev")
button("item_next", "Items Next")
readout("item_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random Pick")

# --- added in 0.0.3 (appended: MPC keeps parameters by index) ---
num("o2_beat", "Beat Freq", "lin", -3.5, 3.5, 0, "beathz")   # dsp/synth.h kBeatRange: Hz on every note
for b in (1, 2):   # what scales the bus's depth, added to its Control (the original's MOD x CONTROL amounts)
    p = "m%d_" % b
    num(p + "wheel", "M%d Wheel" % b, "lin", -1, 1, 0, "bipct")
    num(p + "vel", "M%d Velocity" % b, "lin", -1, 1, 0, "bipct")
    num(p + "at", "M%d Pressure" % b, "lin", -1, 1, 0, "bipct")

# --- added in 0.0.4 ---
for b in (1, 2):   # the rate follows the key (the original's LFO KBTRACK)
    num("m%d_kbt" % b, "M%d Key Track" % b, "lin", 0, 2, 0, "pct")
for e, name in (("fe", "FEG"), ("ae", "AEG")):   # dsp/env.h, dsp/synth.h EnvPatch; the same keys after the prefix
    enum(e + "_exp", name + " Attack Curve", ["Linear", "Exp"], "Linear")
    enum(e + "_latch", name + " Latch", ["Off", "Latch"], "Off")
    enum(e + "_sync", name + " Sync", ["Off"] + SYNC_DIVS, "Off")
    popup_flag(e + "_sync")
enum("o2_kb", "Osc 2 Keys", ["Priority", "High", "Low", "Drone"], "Priority")   # dsp/synth.h Osc2Keys
enum("bend_dest", "Bend To", ["Osc 1+2", "Osc 1", "Osc 2", "Off"], "Osc 1+2")    # dsp/synth.h BendDest


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    if p["curve"] == "pow":
        return (d / hi) ** (1.0 / 3.0) if hi > 0 else 0.0
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        k = p["kind"]
        e = {"key": p["key"], "name": p["name"]}
        if k == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif k == "stepper":
            e.update(min=0, max=1, display="string", type="stepper")
        elif k == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif k == "popup":
            e.update(options=p["options"], default=p["options"][0], popup_of=p["popup_of"], type="enum")
        elif k == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif "options" in p:
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}




# --- touchscreen pages -------------------------------------------------------------------------
# PolyForce's look (style=td3 rounded cards on one flat colour; bg and box the same, so the opaque image of a
# control never shows a box behind it), in SubForce's amber. Plugin area 1280x628 at y = 86..714. Every tab: a
# header row (the status line from x=24), then cards at y=158 and y=440 (h=270) or one full-height card
# (h=552), x=24 w=1232 or halves at x=24 / 648 (w=608). Nothing may sit in a card's title band (y .. y+44: td3
# draws the title rule at y+38). skin_polish.py (run by `make skin` after the generator) redraws the knob
# strips, the trigger buttons and the stepper arrows.
PALETTE = {
    "bg": "16171b", "box": "16171b", "line": "2e3037", "ink": "eee9e1", "ink_dim": "9d968b",
    "ink_faint": "292a30", "accent": "f2a541", "accent_hi": "ffd590", "lcd": "0c0d10",
    "seg_inactive": "212329", "seg_active": "f2a541", "seg_active_tx": "2b1800", "tile_on": "3d2c13",
    "btn_bg": "2b2d34", "title": "b4ac9f", "knob_face": "26282e", "knob_ring": "3c3f47", "knob_dot": "f2a541",
}
FONT_LABEL = "fonts/TitilliumWeb-SemiBold.ttf"   # font_label=: shadow_skin sizes the buttons with it
LIVE_FONT = "fonts/TitilliumWeb-SemiBold.ttf"    # the face MPC draws live text in (names, values)
TITLE_FONT = "fonts/TitilliumWeb-Bold.ttf"       # = SHADOW_TITLE_FONT in the Makefile: card titles, enum
                                                 # labels and segment, popup-option and button text
TITLE_SIZE = 17
THEME = ("style=td3\nfont_label=%s\ntitle_size=%d\n" % (FONT_LABEL, TITLE_SIZE)
         + "".join("theme_%s=%s\n" % kv for kv in PALETTE.items()))
TEXT_INK = PALETTE["ink_dim"]   # free bitmap text (column headers, hints)

S8 = [100, 252, 404, 556, 708, 860, 1012, 1164]   # 8 knob slots across a card = one Q-Link bank
S9 = [96 + 136 * k for k in range(9)]             # 9 slots (an envelope card)
L4, R4 = S8[:4], [724, 876, 1028, 1180]           # 4 slots in the left / right half card
R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552

# Knobs: shadow_skin bakes ONE filmstrip per radius, so the radius picks the look. A bipolar knob (its arc
# grows from 12 o'clock) is one pixel smaller than a unipolar knob of the same size. This table is the only
# place that says so: check_layout() holds every knob to it and skin_polish.py draws the strips from it
# (exported as build/skin_style.json).
KNOB_SIZES = {"big": 30, "small": 22}
KNOB_STYLES = {r - b: {"bipolar": bool(b), "track": 4 if r - b >= 28 else 3, "pointer": 3.0 if r - b >= 28 else 2.5}
               for r in KNOB_SIZES.values() for b in (0, 1)}
BIPOLAR_EXTRA = ()                                # every bipolar knob here has a symmetric range
PRIMARY_BUTTONS = ("pre_save",)                   # drawn in the accent colour by skin_polish.py
FRAMES = 128                                      # shadow_skin: every filmstrip has 128 frames
PARAMS = {p["key"]: p for p in P}


def bipolar(key):
    """A knob whose value runs both ways from the middle (pan, fine, amounts): symmetric range."""
    p = PARAMS[key]
    return p["lo"] == -p["hi"] or key in BIPOLAR_EXTRA


def knob_radius(key, size="big"):
    return KNOB_SIZES[size] - (1 if bipolar(key) else 0)


class Layout:
    """layout.conf lines. mode() tags every widget that follows with when= until the next tab or mode()."""

    def __init__(self):
        self.lines, self.when = [THEME], None

    def tab(self, name):
        self.lines.append("[tab %s]" % name)
        self.when = None

    def mode(self, when):
        self.when = when

    def add(self, line):
        self.lines.append(line + (' when="%s"' % self.when if self.when else ""))

    def header(self, modes=None, status_w=None):
        """The status line from x=24 and the page-mode selector right-aligned to x=1256."""
        n = len(PARAMS[modes]["options"]) if modes else 0
        w = status_w or (1232 - n * 124 - 16 if n else 1232)
        self.readout(24 + w // 2, 121, w, "status")
        if n:
            self.hseg(1256 - (n * 124 - 2) // 2, 121, modes, 122)

    def card(self, x, y, w, h, title):
        self.add('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, w, h, title))

    def knob(self, cx, cy, key, size="big"):
        self.add('knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, knob_radius(key, size), PARAMS[key]["name"], key))

    def hseg(self, cx, cy, key, sw, label=None):
        self.add('enum_h cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def stepper(self, cx, cy, w, key):
        self.add('stepper cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def readout(self, cx, cy, w, key, h=40):
        self.add('readout cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def button(self, cx, cy, label, key):
        self.add('button cx=%d cy=%d label="%s" key=%s' % (cx, cy, label, key))

    def text(self, cx, top, label):   # bitmap font; top = the top of the glyphs (render_conf_preview.c)
        self.add('text cx=%d cy=%d label="%s" color=%s' % (cx, top, label, TEXT_INK))

    def slider(self, cx, cy, w, h, cw, key):
        self.add('slider_v cx=%d cy=%d w=%d h=%d cw=%d label="%s" key=%s' % (cx, cy, w, h, cw, PARAMS[key]["name"], key))

    def toggle(self, cx, cy, key):
        self.add('toggle cx=%d cy=%d label="%s" key=%s' % (cx, cy, PARAMS[key]["name"], key))

    def meter(self, cx, cy, w, h, key):   # display only: shadow_skin's filmstrip meter, no look (PolyForce patch)
        self.add('meter cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def tiles(self, x, y, w, cols, rows, th, gap, key):
        self.add('list x=%d y=%d w=%d cols=%d rows=%d th=%d gap=%d key=%s' % (x, y, w, cols, rows, th, gap, key))

    def qlinks(self, title, keys):
        """A Q-Link set: MPC shows its title in the tab strip. Sets only remap the Q-Links (the screen stays),
        so each is named after what it controls and makes sense whatever page mode is showing."""
        self.lines.append('qlinks "%s" = %s' % (title, ",".join(keys)))




def env_card(L, top, e, title):
    """A DAHDSR envelope: six knobs; Loop over Reset, Latch over the attack curve; Sync."""
    L.card(24, top, 1232, 270, title)
    for cx, k in zip(S9, ("dly", "a", "hold", "d", "s", "r")):
        L.knob(cx, top + 126, "%s_%s" % (e, k))
    for cx, (k1, l1), (k2, l2) in ((S9[6], ("loop", "LOOP"), ("reset", "RESET")),
                                   (S9[7], ("latch", "LATCH"), ("exp", "ATTACK"))):
        L.vseg(cx, top + 112, "%s_%s" % (e, k1), label=l1)
        L.vseg(cx, top + 212, "%s_%s" % (e, k2), label=l2)
    L.text(S9[8], top + 86, "SYNC")
    L.popup(S9[8], top + 126, 120, e + "_sync")


def pages():
    L = Layout()

    # OSC: the two oscillators side by side, the mixer below.
    L.tab("OSC")
    L.header()
    L.card(24, R1, 456, 270, "OSCILLATOR 1")
    L.vseg(S8[0], R1 + 160, "o1_oct", label="OCTAVE")
    L.knob(S8[1], R1 + 126, "o1_wave")
    L.vseg(S8[2], R1 + 160, "sub_oct", label="SUB OSC")
    L.card(496, R1, 760, 270, "OSCILLATOR 2")
    for cx, k in zip((572,) + tuple(R4), ("o2_oct", "o2_freq", "o2_beat", "o2_wave", "o2_sync")):
        if k in ("o2_oct", "o2_sync"):
            L.vseg(cx, R1 + 160, k, label="OCTAVE" if k == "o2_oct" else "HARD SYNC")
        else:
            L.knob(cx, R1 + 126, k)
    L.card(24, R2, 1232, 270, "MIXER")
    for cx, k in zip(S8, ("mix_o1", "mix_sub", "mix_o2", "mix_noise", "mix_fb", "noise_color", "drift")):
        L.knob(cx, R2 + 126, k)
    L.vseg(S8[7], R2 + 160, "kb_reset", label="KB RESET")
    L.qlinks("OSC + MIX", ["o1_oct", "o1_wave", "o2_oct", "o2_freq", "o2_beat", "o2_wave", "o2_sync", "sub_oct",
                           "mix_o1", "mix_sub", "mix_o2", "mix_noise", "mix_fb", "noise_color", "drift", "kb_reset"])

    # FILTER: the ladder (and how the filter EG reaches it), the filter EG below.
    filt = ("f_cut", "f_res", "f_drive", "f_env", "f_kb", "fe_vel", "fe_kb")
    L.tab("FILTER")
    L.header()
    L.card(24, R1, 1232, 270, "FILTER")
    for cx, k in zip(S8, filt):
        L.knob(cx, R1 + 126, k)
    L.vseg(S8[7], R1 + 160, "f_slope", label="SLOPE")
    env_card(L, R2, "fe", "FILTER EG")
    L.qlinks("FILTER", list(filt) + ["f_slope"] + ["fe_" + k for k in ("dly", "a", "hold", "d", "s", "r", "loop", "reset")])

    # AMP: the amp EG, the VCA.
    L.tab("AMP")
    L.header()
    env_card(L, R1, "ae", "AMP EG")
    L.card(24, R2, 1232, 270, "VCA")
    for cx, k in zip(S8, ("ae_vel", "ae_kb", "volume")):
        L.knob(cx, R2 + 126, k)
    L.qlinks("AMP", ["ae_" + k for k in ("dly", "a", "hold", "d", "s", "r", "loop", "reset", "vel", "kb")]
             + ["volume", "f_cut", "f_res", "f_env", "fe_d", "glide"])

    # MOD: both busses. Top row: what drives it; bottom row: where it goes.
    L.tab("MOD")
    L.header()
    for b, top in ((1, R1), (2, R2)):
        p = "m%d_" % b
        L.card(24, top, 1232, 270, "MOD %d" % b)
        for cx, label in ((144, "SOURCE"), (590, "SYNC RATE"), (820, "CONTROL")):
            L.text(cx, top + 50, label)
        L.popup(144, top + 88, 200, p + "src")
        L.hseg(380, top + 88, p + "sync", 76)
        L.popup(590, top + 88, 170, p + "div")
        L.popup(820, top + 88, 190, p + "ctl")
        L.hseg(1110, top + 88, p + "trig", 110)
        L.knob(100, top + 176, p + "rate")
        L.knob(252, top + 176, p + "pitch")
        L.text(404, top + 118, "PITCH TO")
        L.vseg(404, top + 182, p + "pdest")
        L.knob(556, top + 176, p + "filter")
        L.text(760, top + 150, "DESTINATION")
        L.popup(760, top + 190, 200, p + "dest")
        L.knob(940, top + 176, p + "amt")
        L.knob(S8[7], top + 176, p + "kbt")
    L.qlinks("MOD 1+2", ["m%d_%s" % (b, k) for b in (1, 2)
                         for k in ("rate", "pitch", "filter", "amt", "src", "dest", "ctl", "sync")])

    # BROWSE: categories left, presets right, the loaded preset and actions below.
    L.tab("BROWSE")
    L.header()
    L.card(24, R1, 360, 552, "CATEGORIES")
    L.tiles(44, 206, 320, 2, 8, 48, 8, "cat")
    L.button(124, 676, "< PREV", "cat_prev")
    L.button(304, 676, "NEXT >", "cat_next")
    L.card(400, R1, 856, 474, "PRESETS")
    L.tiles(420, 206, 816, 3, 8, 38, 8, "item")
    L.button(476, 596, "< PREV", "item_prev")
    L.readout(828, 596, 240, "item_page", h=36)
    L.button(1180, 596, "NEXT >", "item_next")
    L.readout(590, 676, 380, "br_now")   # the row: 400..1256, 8 px apart
    L.toggle(848, 668, "fav")
    L.button(968, 676, "RND", "rnd")
    L.button(1083, 676, "SAVE", "pre_save")
    L.button(1197, 676, "INIT", "pre_init")
    L.qlinks("BROWSE", ["preset", "f_cut", "f_res", "f_drive", "f_env", "o1_wave", "o2_wave", "o2_freq",
                        "mix_o1", "mix_sub", "mix_o2", "fe_d", "ae_d", "glide", "rand_amt", "volume"])

    # KEYS: how keys become notes; glide; the patch.
    L.tab("KEYS")
    L.header()
    L.card(24, R1, 1232, 270, "KEYBOARD")
    L.hseg(200, R1 + 96, "kmode", 110, label="MODE")
    L.hseg(600, R1 + 96, "prio", 110, label="PRIORITY")
    L.hseg(1000, R1 + 96, "trig", 120, label="TRIGGER")
    for cx, k in zip(S8, ("bend_up", "bend_dn")):
        L.knob(cx, R1 + 172, k)
    L.hseg(560, R1 + 190, "bend_dest", 100, label="BEND TO")
    L.hseg(1020, R1 + 190, "o2_kb", 100, label="OSC 2 KEYS")
    L.card(24, R2, 608, 270, "GLIDE")
    L.vseg(100, R2 + 160, "glide_mode", label="GLIDE")   # five options: its top lines up with the others'
    L.vseg(240, R2 + 128, "glide_type", label="TYPE")
    L.vseg(380, R2 + 128, "glide_dest", label="OSC")
    L.knob(530, R2 + 126, "glide")
    L.card(648, R2, 608, 270, "PATCH")
    L.stepper(888, R2 + 76, 440, "preset")
    L.button(730, R2 + 204, "SAVE", "pre_save")
    L.button(846, R2 + 204, "INIT", "pre_init")
    L.button(980, R2 + 204, "RANDOM", "pre_rand")
    L.knob(1186, R2 + 76, "rand_amt", "small")
    L.qlinks("KEYS", ["kmode", "prio", "trig", "o2_kb", "glide_mode", "glide_type", "glide", "glide_dest",
                      "bend_up", "bend_dn", "bend_dest", "preset", "rand_amt", "volume"])

    # DEPTH: what scales each bus (its Control, plus the wheel, velocity and pressure amounts).
    L.tab("DEPTH")
    L.header()
    for b, top in ((1, R1), (2, R2)):
        p = "m%d_" % b
        L.card(24, top, 1232, 270, "MOD %d DEPTH" % b)
        L.text(144, top + 86, "CONTROL")
        L.popup(144, top + 126, 190, p + "ctl")
        for cx, k in zip(S8[2:5], ("wheel", "vel", "at")):
            L.knob(cx, top + 126, p + k)
        L.text(1012, top + 112, "THE DEPTH: CONTROL")
        L.text(1012, top + 142, "+ WHEEL + VELOCITY + PRESSURE")
    L.qlinks("DEPTH", ["m%d_%s" % (b, k) for b in (1, 2) for k in ("ctl", "wheel", "vel", "at")])
    return "\n".join(L.lines) + "\n"


def skin_style():
    """What skin_polish.py needs to redraw the knob strips, buttons and stepper arrows (build/skin_style.json)."""
    return {"palette": PALETTE, "title_font": TITLE_FONT, "frames": FRAMES,
            "knobs": {str(r): s for r, s in sorted(KNOB_STYLES.items())}, "primary_buttons": list(PRIMARY_BUTTONS)}


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
# Geometry mirrors third_party/mpc-vst-plugins/tools/shadow_skin.py (component boxes, button_rect,
# seg_rects, popup_layout) and render_conf_preview.c (the bitmap font of `text`).
X0, Y0, X1, Y1 = 0, 86, 1280, 714
TITLE_BAND = 44              # a card's title band: y .. y+44
NAME_MAX = 13                # MPC shows a knob/slider's effGetParamName at ~19.5 px in a 130 px box
POP_ROW, POP_GAP, POP_PAD, POP_GROUP_ROWS = 40, 2, 6, 8
BITMAP_GLYPHS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/_>%+:#"   # font8x8.h font_chars
BITMAP_ADVANCE = {" ": 4, "J": 9, "j": 9, ".": 9, "-": 9, ":": 9}   # font_glyph_width(): last lit column + 2; else 10
INT_KEYS = ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw")


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in INT_KEYS else v
    return w


def bitmap_width(s, scale):
    """render_conf_preview.c text_width()."""
    return int(sum(BITMAP_ADVANCE.get(c, 10) * scale for c in s))


_FONTS = {}


def _ttf_advances(path):
    """(unitsPerEm, {char: advance}) from a TrueType font's cmap (format 4) and hmtx tables."""
    import struct
    d = open(path, "rb").read()
    tabs = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag.decode("latin-1")] = off
    upem = struct.unpack(">H", d[tabs["head"] + 18:tabs["head"] + 20])[0]
    nhm = struct.unpack(">H", d[tabs["hhea"] + 34:tabs["hhea"] + 36])[0]
    adv = [struct.unpack(">H", d[tabs["hmtx"] + 4 * i:tabs["hmtx"] + 4 * i + 2])[0] for i in range(nhm)]
    co = tabs["cmap"]
    for i in range(struct.unpack(">H", d[co + 2:co + 4])[0]):
        pid, eid, off = struct.unpack(">HHI", d[co + 4 + 8 * i:co + 12 + 8 * i])
        so = co + off
        if struct.unpack(">H", d[so:so + 2])[0] != 4 or (pid, eid) not in ((3, 1), (0, 3), (0, 4)):
            continue
        n2 = struct.unpack(">H", d[so + 6:so + 8])[0]
        ends = struct.unpack(">%dH" % (n2 // 2), d[so + 14:so + 14 + n2])
        starts = struct.unpack(">%dH" % (n2 // 2), d[so + 16 + n2:so + 16 + 2 * n2])
        deltas = struct.unpack(">%dh" % (n2 // 2), d[so + 16 + 2 * n2:so + 16 + 3 * n2])
        ro = so + 16 + 3 * n2
        ranges = struct.unpack(">%dH" % (n2 // 2), d[ro:ro + n2])
        out = {}
        for s in range(n2 // 2):
            for c in range(starts[s], min(ends[s], 0x7e) + 1):
                if ranges[s]:
                    gi = ro + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[gi:gi + 2])[0]
                    g = (g + deltas[s]) & 0xFFFF if g else 0
                else:
                    g = (c + deltas[s]) & 0xFFFF
                out[chr(c)] = adv[min(g, nhm - 1)]
        return upem, out
    raise SystemExit("%s: no Unicode cmap" % path)


def ttf_width(font, px, s):
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does. Without
    Pillow (surface.py needs only python3): from the font's own advance table, +1 px (that is within 0.75 px
    of Pillow for Titillium Web, and errs wide)."""
    key = (font, px)
    if key not in _FONTS:
        path = os.path.join(HERE, font)
        try:
            from PIL import ImageFont
            _FONTS[key] = ImageFont.truetype(path, px).getlength
        except ImportError:
            upem, adv = _ttf_advances(path)
            _FONTS[key] = lambda t: sum(adv.get(c, upem) for c in t) * px / upem + 1.0
    return _FONTS[key](s)


def _top_level(text):
    """The style keys before the first [tab] (shadow_skin apply_theme)."""
    top = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            break
        k, eq, v = line.partition("=")
        if eq and not line.startswith("#"):
            top[k.strip()] = v.strip()
    return top


class Geometry:
    """Where shadow_skin puts each widget, for this layout's style keys."""

    def __init__(self, top):
        self.td3 = top.get("style") == "td3"
        self.font_label = top.get("font_label")
        self.ls = float(top.get("label_scale", 1.15))

    def knob(self, w):   # shadow_skin build(): the filmstrip, the Name and Value labels under it
        r = w["r"]
        s = 2 * r + 10
        cw = max(130, s)
        name_y = s // 2 + r + 2
        ch = name_y + round(20 * self.ls) + 2 + round(26 * self.ls) + 6
        return (w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch)

    def slider(self, w):
        sq = max(w["w"], w["h"])
        cw = w.get("cw", max(130, sq))
        ch = (sq - w["h"]) // 2 + w["h"] + 2 + 20 + 2 + 26 + 6
        return (w["cx"] - cw // 2, w["cy"] - sq // 2, cw, ch)

    def text_width(self, s):   # shadow_skin text_width(): sizes a button
        if self.font_label:
            return int(ttf_width(self.font_label, round(9 * 1.15 * 1.6), s) * 1.2)
        return int(len(s) * 10 * 1.15 - 1.15)

    def button(self, w):   # shadow_skin button_rect()
        bw, bh = self.text_width(w["label"]) + 36, 39
        if self.td3:
            bw, bh = bw + 24 + 4, 48 + 4
        return (w["cx"] - bw // 2, w["cy"] - bh // 2, bw, bh)

    @staticmethod
    def segs(w, n):   # shadow_skin seg_rects()
        if w["kind"] == "enum_v":
            sw = w.get("sw") or 135
            y0 = w["cy"] - (n * 32) // 2
            return [(w["cx"] - sw // 2, y0 + i * 32, sw, 30) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * 2) // 2 + c * (sw + 2), w["cy"] - 16 + r * 35, sw, 33))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = w["cy"] - 33 // 2 - 22 if w["kind"] == "enum_h" else w["cy"] - (n * 32) // 2 - 24
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", 1.5))
        tw = bitmap_width(w["label"], size)
        return (w["cx"] - tw // 2, w["cy"], tw + 1, int(9 * size + 0.5))

    @staticmethod
    def popup_panel(w, n):   # shadow_skin popup_layout(): the open list
        fx, fy, fw, fh = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"]
        below, above = Y1 - (fy + fh + 4), fy - 4 - Y0
        groups = [(t, int(c)) for t, _, c in (g.rpartition(":") for g in w["groups"].split(","))] \
            if w.get("groups") else None
        if groups:
            rows = min(POP_GROUP_ROWS, max(c for _, c in groups))
            cols = sum(-(-c // rows) for _, c in groups)
            ph = (rows + 1) * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
        else:
            for cols in ([int(w["cols"])] if w.get("cols") else range(1, n + 1)):
                rows = -(-n // cols)
                ph = rows * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
                if ph <= max(below, above):
                    break
        pw = cols * fw + (cols - 1) * POP_GAP + 2 * POP_PAD
        py = fy + fh + 4 if ph <= below else fy - 4 - ph if ph <= above else Y0
        return (max(0, min(fx, X1 - pw)), py, pw, ph)

    def rects(self, w, params):
        """[(x, y, w, h)] of a control, as shadow_skin places it (stepper: arrows and text together)."""
        k = w["kind"]
        if k == "knob":
            return [self.knob(w)]
        if k in ("slider_v", "slider_h"):
            return [self.slider(w)]
        if k == "toggle":
            return [(w["cx"] - 60, w["cy"] - 18, 120, 58)]
        if k == "button":
            return [self.button(w)]
        if k in ("readout", "stepper", "popup", "menu"):
            return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
        if k == "list":
            tw = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"]
            return [(w["x"] + c * (tw + w["gap"]), w["y"] + r * (w["th"] + w["gap"]), tw, w["th"])
                    for r in range(w["rows"]) for c in range(w["cols"])]
        if k in ("enum_h", "enum_v"):
            return self.segs(w, len(params[w["key"]]["options"]))
        if k == "meter":   # shadow_skin: a square of the larger side, centred (transparent padding)
            sq = max(w["w"], w["h"])
            return [(w["cx"] - sq // 2, w["cy"] - sq // 2, sq, sq)]
        return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _inside(r):
    return r[0] >= X0 and r[1] >= Y0 and r[0] + r[2] <= X1 and r[1] + r[3] <= Y1


def _same_screen(m1, m2):
    return m1 is None or m2 is None or m1 == m2


def check_names(layout_tabs, geo, errors):
    """Parameter names: MPC shows them under knobs and sliders and in its Q-Link overlay, without page context."""
    seen = {}
    for p in P:
        if len(p["name"]) > 24:
            errors.append("parameter %s: name %r is longer than 24 characters" % (p["key"], p["name"]))
        if p["name"].lower() in seen:
            errors.append("parameters %s and %s have the same name %r" % (seen[p["name"].lower()], p["key"], p["name"]))
        seen[p["name"].lower()] = p["key"]
    for tab in layout_tabs:
        for w in tab["widgets"]:
            if w["kind"] not in ("knob", "slider_v", "slider_h", "toggle") or w.get("key") not in PARAMS:
                continue
            name = PARAMS[w["key"]]["name"]
            if len(name) > NAME_MAX:
                errors.append("%s: %s %s: name %r is longer than %d characters" % (tab["name"], w["kind"], w["key"], name,
                                                                                    NAME_MAX))
            # the live Name label: knob 17 x label_scale px in max(130, 2r+10); slider 17 px in cw; toggle 15 px in 120
            px, box = ((math.ceil(17 * geo.ls), max(130, 2 * w["r"] + 10)) if w["kind"] == "knob" else
                       (15, 120) if w["kind"] == "toggle" else (17, w.get("cw") or max(130, w["w"], w["h"])))
            if ttf_width(LIVE_FONT, px, name) > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label" % (tab["name"], w["kind"], w["key"], name, box))


def check_layout(text):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, unknown bitmap glyphs."""
    params = PARAMS
    geo = Geometry(_top_level(text))
    errors = []
    tabs = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or (not tabs and "=" in line and not line.startswith("[")):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1), "widgets": [], "qlinks": []})
            continue
        if line.startswith("qlinks"):
            m = re.match(r'qlinks\s+"([^"]+)"\s*=\s*(.+)$', line)
            keys = [k.strip() for k in m.group(2).split(",") if k.strip()]
            tabs[-1]["qlinks"].append((m.group(1), keys))
            continue
        tabs[-1]["widgets"].append(_widget(line))
    if len(tabs) > 7:
        errors.append("%d tabs: MPC shows five plus a pager; keep it to seven" % len(tabs))
    check_names(tabs, geo, errors)
    errors += ["PRIMARY_BUTTONS: %r is not a button parameter" % k for k in PRIMARY_BUTTONS
               if PARAMS.get(k, {}).get("kind") != "button"]
    errors += ["BIPOLAR_EXTRA: %r is not a parameter" % k for k in BIPOLAR_EXTRA if k not in PARAMS]
    seg_images = {}   # shadow_skin names enum images sh_seg_<key>_<n> for the whole skin: one size per key
    for tab in tabs:
        T = tab["name"]
        frames, placed = [], []   # (rect, title, mode); (rect, what, mode)
        for w in tab["widgets"]:
            kind, key, mode = w["kind"], w.get("key"), w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (T, mode))
            if kind == "frame":
                r = (w["x"], w["y"], w["w"], w["h"])
                if not _inside(r):
                    errors.append("%s: frame %r at %s leaves the plugin area" % (T, w.get("title"), r))
                frames.append((r, w.get("title", ""), mode))
                continue
            if kind == "text":
                lab = w.get("label", "")
                bad = sorted(set(c for c in lab if c not in BITMAP_GLYPHS))
                if not lab or bad:
                    errors.append("%s: text %r: %s" % (T, lab, "the bitmap font has no %r" % "".join(bad) if bad
                                                       else "an empty label fails shadow_art"))
                placed.append((Geometry.text(w), "text %r" % lab, mode))
                continue
            if kind == "art":
                errors.append("%s: art needs the browser renderer" % T)
                continue
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            missing = [k for k in need if k not in params]
            for k in missing:
                errors.append("%s: %s key %r is not a parameter" % (T, kind, k))
            if missing:
                continue
            p = params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (T, kind, key))
                continue
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (T, key))
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (T, key))
            if kind == "meter" and p["kind"] != "meter":
                errors.append("%s: meter %r is not a meter parameter" % (T, key))
            if kind == "button" and not w.get("label"):
                errors.append("%s: button %r needs a label" % (T, key))
                continue
            if kind == "knob":
                style = KNOB_STYLES.get(w["r"])
                if not style:
                    errors.append("%s: knob %s: r=%d has no look in KNOB_STYLES" % (T, key, w["r"]))
                elif style["bipolar"] != bipolar(key):
                    errors.append("%s: knob %s: r=%d is a %s look, the parameter is %s" % (
                        T, key, w["r"], "bipolar" if style["bipolar"] else "unipolar",
                        "bipolar" if bipolar(key) else "unipolar"))
            if kind in ("readout", "stepper", "popup") and w["h"] < 36:
                errors.append("%s: %s %s: h=%d clips its 26 px live text (min 36)" % (T, kind, key, w["h"]))
            if kind in ("enum_h", "enum_v"):
                n = len(p["options"])
                size = (kind, w.get("sw"), n)
                if seg_images.setdefault(key, size) != size:
                    errors.append("%s: enum %s is drawn as %s and %s: its segment images are shared" % (
                        T, key, seg_images[key], size))
                if w.get("label"):
                    placed.append((Geometry.enum_label(w, n), "%s label" % key, mode))
            if kind == "popup":
                panel = Geometry.popup_panel(w, len(p["options"]))
                if not _inside(panel):
                    errors.append("%s: popup %s: its open list %s leaves the plugin area" % (T, key, panel))
            for r in geo.rects(w, params):
                placed.append((r, "%s %s" % (kind, key), mode))
        for i, (r, what, mode) in enumerate(placed):
            if not _inside(r):
                errors.append("%s: %s at %s leaves the plugin area" % (T, what, r))
            for o_r, o_what, o_mode in placed[:i]:
                if what.startswith("meter ") and o_what.startswith("meter "):
                    continue   # a row of meters: their padded squares overlap, transparent and untouchable
                if _same_screen(mode, o_mode) and _overlap(r, o_r) and o_what != what:
                    errors.append("%s: %s overlaps %s" % (T, what, o_what))
            for f_r, title, f_mode in frames:
                band = (f_r[0], f_r[1], f_r[2], TITLE_BAND)
                if _same_screen(mode, f_mode) and _overlap(r, band):
                    errors.append("%s: %s at %s is in the title band of card %r" % (T, what, r, title))
        titles = [t for t, _ in tab["qlinks"]]
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (T, title, len(keys)))
            if len(title) > 12 or titles.count(title) > 1:
                errors.append("%s: qlinks title %r: keep it unique and at most 12 characters (MPC's tab strip)" % (T, title))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (T, title, k))
    if errors:
        raise SystemExit("layout check failed:\n  " + "\n  ".join(errors))
    return tabs




# --- C++ header --------------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int", "pow": "Pow"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "bipct": "Bipolar", "hz": "Hz", "time": "Time",
       "semi": "Semi", "count": "Count", "db": "Db", "text": "Text", "lfohz": "LfoHz", "wave": "Wave",
       "semifine": "SemiFine", "envamt": "EnvAmt", "modpitch": "ModPitch", "modcut": "ModCut", "noise": "Noise",
       "range": "Range", "beathz": "BeatHz"}
KIND = {"synth": "Synth", "ui": "Ui", "readout": "Readout", "stepper": "Stepper", "button": "Button",
        "tile": "Tile", "toggle": "Toggle", "popup": "Popup", "meter": "Meter"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    index = {p["key"]: i for i, p in enumerate(P)}
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    opts = []
    for i, p in enumerate(P):
        if "options" in p:
            opts.append("static constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1) for i, p in enumerate(P))
    uid = int.from_bytes(VST["uid"].encode(), "big")
    return """// generated by surface/surface.py: do not edit
#pragma once
#include <cstdint>

namespace sf {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int, Pow };
enum class Fmt : unsigned char { %s };
// Who owns the value and what a set does: see surface.py "kind".
enum class Kind : unsigned char { Synth, Ui, Readout, Stepper, Button, Tile, Toggle, Popup, Meter };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    float def;                  // MPC's 0..1 default
    int nopts;
    const char* const* opts;
    int popupOf;                // Kind::Popup: the parameter whose list it opens, else -1
};

static constexpr ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static constexpr ParamInfo PARAM_INFO[P_COUNT] = {
%s
};

constexpr const char* kPlugName = %s;
constexpr const char* kPlugVendor = %s;
constexpr int32_t kPlugUid = 0x%08x;   // '%s'
constexpr int32_t kPlugVersion = %d;

constexpr int kStepperRange = %d;
constexpr int kBrowserCats = %d;
constexpr int kBrowserItems = %d;
constexpr int kNumOctaves = %d;
constexpr int kNumSlopes = %d;
constexpr int kNumModSources = %d;
constexpr int kNumModDests = %d;
constexpr int kNumModControls = %d;
constexpr int kNumSyncDivisions = %d;

} // namespace sf
""" % (ids, ", ".join(dict.fromkeys(FMT.values())), specs, "\n".join(opts), info, c_str(VST["name"]),
       c_str(VST["vendor"]), uid, VST["uid"], VST["version"], STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS,
       len(OCTAVES), len(SLOPES), len(MOD_SOURCES), len(MOD_DESTS), len(MOD_CONTROLS), len(SYNC_DIVS))


# --- factory presets: presets/Factory/<NN_Category>/<NN_Name>.sfp, embedded in the .so ---------
PRESET_DIR = os.path.join(HERE, "..", "presets", "Factory")
PRESET_MAGIC = "subforce "
NUMBER = re.compile(r"^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$")
PRESET_NAME_MAX = 18      # an item tile on the browser page (816 px / 3 columns)
CATEGORY_NAME_MAX = 12    # a category tile (320 px / 2 columns), shown in capitals


def _shown(entry):
    """"02_Rubber_Bass.sfp" -> "Rubber Bass", "03_Bass" -> "Bass"."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.sfp$", "", entry).replace("_", " "))


def factory_presets():
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). Every line must be a sound parameter with a value in range, every name unique (keys are
    "builtin:<name>") and short enough for its tile: a typo fails the build, not the device."""
    params = {p["key"]: p for p in P}
    out, errors, seen = [], [], {}
    for d in sorted(os.listdir(PRESET_DIR)):
        folder = os.path.join(PRESET_DIR, d)
        if d.endswith(".sfp"):
            errors.append("%s: put it in a category folder (presets/Factory/NN_Category/)" % d)
            continue
        if not os.path.isdir(folder):
            continue
        category = _shown(d)
        if not category or len(category) > CATEGORY_NAME_MAX:
            errors.append("%s: a category name of 1..%d characters" % (d, CATEGORY_NAME_MAX))
        for f in sorted(os.listdir(folder)):
            if not f.endswith(".sfp"):
                continue
            where = "%s/%s" % (d, f)
            text = open(os.path.join(folder, f), encoding="utf-8").read().replace("\r\n", "\n")
            lines = text.split("\n")
            if lines[0] != PRESET_MAGIC + "1":
                errors.append("%s: the first line must be '%s1'" % (where, PRESET_MAGIC))
            keys = set()
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
                p = params.get(key)
                if not p or p["kind"] != "synth":
                    errors.append("%s:%d: %r is not a sound parameter" % (where, n, key))
                    continue
                if key in keys:
                    errors.append("%s:%d: %s given twice" % (where, n, key))
                keys.add(key)
                if not NUMBER.match(val):   # what the plugin's parser (std::from_chars) reads, no more
                    errors.append("%s:%d: %r is not a number" % (where, n, val))
                    continue
                v = float(val)
                lo, hi = p["lo"], p["hi"]
                if not (min(lo, hi) - 1e-9 <= v <= max(lo, hi) + 1e-9):
                    errors.append("%s:%d: %s=%s outside %s..%s" % (where, n, key, val, lo, hi))
                if p["curve"] in ("int", "enum") and v != round(v):
                    errors.append("%s:%d: %s=%s is not a whole number" % (where, n, key, val))
            name = _shown(f)
            if name in seen:
                errors.append("%s: the name %r is taken by %s" % (where, name, seen[name]))
            seen[name] = where
            if len(name) > PRESET_NAME_MAX:
                errors.append("%s: %r is longer than %d characters" % (where, name, PRESET_NAME_MAX))
            out.append((category, name, text))
    if "Init" not in seen:
        errors.append("no Init preset (the INIT button loads builtin:Init)")
    if errors:
        raise SystemExit("factory presets:\n  " + "\n  ".join(errors))
    return out


def presets_header(presets):
    rows = ",\n".join("    {%s, %s, %s}" % (c_str(c), c_str(n), c_str(t).replace("\n", "\\n")) for c, n, t in presets)
    return """// generated by surface/surface.py from presets/Factory/*/*.sfp: do not edit
#pragma once

namespace sf {

struct FactoryPreset { const char* category; const char* name; const char* text; };
static const FactoryPreset kFactoryPresets[] = {
%s
};
constexpr int kNumFactoryPresets = %d;

} // namespace sf
""" % (rows, len(presets))


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    layout = pages()
    check_layout(layout)
    presets = factory_presets()   # everything checked before anything is written
    outputs = [
        ("params.json", json.dumps(params_json(), indent=1)),
        ("layout.conf", layout),
        ("vst.json", json.dumps(VST, indent=1)),
        (os.path.join("build", "skin_style.json"), json.dumps(skin_style(), indent=1)),
        (os.path.join("build", "factory_presets.h"), presets_header(presets)),
        (os.path.join("build", "param_ids.h"), header()),   # last: make's target, newer than the rest
    ]
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    for name, text in outputs:
        path = os.path.join(HERE, name)
        with open(path + ".tmp", "w", newline="\n") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    print("surface: %d parameters, layout ok, %d factory presets" % (len(P), len(presets)))


if __name__ == "__main__":
    sys.exit(main())
