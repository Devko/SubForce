"""SubForce's preset demo video: the pages as they play (skin.py), the preset and its effect, whether it
is clean or through EffectForce, over the rendered demo (render.cpp's out/mix.wav, state.jsonl,
cues.jsonl), encoded with ffmpeg.

    python compose.py                                   # the whole demo: out/subforce_demo.mp4
    python compose.py --from 3.87 --to 34.84 --name part   # a part (seconds)
    python compose.py --still 70                        # one frame as out/still.png (for checking)
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from skin import Skin  # noqa: E402

OUT = os.path.join(HERE, "out")
VW, VH, FPS = 1920, 1080, 30
DEV = r"D:\DEV"
FONT_BOLD = os.path.join(DEV, "EffectForce", "surface", "fonts", "TitilliumWeb-Bold.ttf")
FONT_SEMI = os.path.join(DEV, "EffectForce", "surface", "fonts", "TitilliumWeb-SemiBold.ttf")
BG = (12, 13, 16)
INK = (233, 233, 240)
DIM = (150, 152, 166)
FAINT = (52, 55, 64)

# instance -> plugin, from the cues ("inst"); every plugin's colour is its skin's accent
INSTANCES = {}
ACCENT = {"PolyForce": (63, 208, 192), "SubForce": (242, 165, 65), "EffectForce": (160, 140, 245)}
PAGE_SCALE = 1.25
PAGE_X, PAGE_Y = (VW - int(1280 * PAGE_SCALE)) // 2, 132
PAGE_W, PAGE_H = int(1280 * PAGE_SCALE), int(628 * PAGE_SCALE)

_fonts = {}


def font(px, bold=True):
    key = (px, bold)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(FONT_BOLD if bold else FONT_SEMI, px)
    return _fonts[key]


def ease(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def mix(c1, c2, a):
    return tuple(int(round(c1[k] + (c2[k] - c1[k]) * a)) for k in range(3))


def text(d, xy, s, px, fill, bold=True, anchor="la"):
    d.text(xy, s, font=font(px, bold), fill=fill, anchor=anchor)


# --- the timeline -----------------------------------------------------------------------------
class Timeline:
    def __init__(self):
        cues = [json.loads(l) for l in open(os.path.join(OUT, "cues.jsonl"), encoding="utf-8")]
        head = cues[0]
        self.bpm, self.seconds = head["bpm"], head["seconds"]
        self.cues = sorted(cues[1:], key=lambda c: c["t"])
        self.sections = [c for c in self.cues if c["type"] == "section"]
        self.pages = [c for c in self.cues if c["type"] == "page"]
        self.captions = [c for c in self.cues if c["type"] == "caption"]
        self.taps = [c for c in self.cues if c["type"] == "tap"]
        self.modes = [c for c in self.cues if c["type"] == "mode"]
        for c in self.cues:
            if c["type"] == "inst":
                INSTANCES[c["i"]] = c["plug"]
        # only the instances the video shows: their skins and states
        shown = {c["i"] for c in self.pages}
        for k in [k for k in INSTANCES if k not in shown]:
            del INSTANCES[k]
        # parameter states per instance, per frame (the log holds changes)
        self.deltas = {}
        for l in open(os.path.join(OUT, "state.jsonl"), encoding="utf-8"):
            d = json.loads(l)
            self.deltas.setdefault(d["i"], []).append((d["f"], d["v"]))
        self.skins = {name: Skin(os.path.join(DEV, name)) for name in set(INSTANCES.values())}
        self.state = {}
        for inst, plug in INSTANCES.items():
            n = len(self.skins[plug].params)
            self.state[inst] = {"vals": [0.0] * n, "texts": [""] * n, "next": 0}

    def advance(self, frame):
        """Brings every instance's state up to `frame` (frames come in order)."""
        for inst, st in self.state.items():
            ds = self.deltas.get(inst, [])
            while st["next"] < len(ds) and ds[st["next"]][0] <= frame:
                for k, (v, t) in ds[st["next"]][1].items():
                    st["vals"][int(k)] = v
                    st["texts"][int(k)] = t
                st["next"] += 1

    def last(self, cues, t):
        found = None
        for c in cues:
            if c["t"] <= t + 1e-6:
                found = c
            else:
                break
        return found

    def section_at(self, t):
        return self.last(self.sections, t)


# --- pieces of the frame ----------------------------------------------------------------------
def page_image(tl, inst, tab, t):
    plug = INSTANCES[inst]
    sk = tl.skins[plug]
    st = tl.state[inst]
    texts = list(st["texts"])
    # The status line's CPU meter would show this PC's numbers, not the Force's: left out.
    for k in ("status",):
        if k in sk.index:
            i = sk.index[k]
            texts[i] = re.sub(r"\s*CPU\s*\d+%\s*PEAK\s*\d+%", "", texts[i]).strip()
    pressed = {c["key"] for c in tl.taps if c["i"] == inst and 0 <= t - c["t"] < 0.18}
    im = sk.page(tab, st["vals"], texts, pressed)
    return im, sk


def framed_page(page):
    """The page scaled into the video, with a border and a soft shadow."""
    big = page.resize((PAGE_W, PAGE_H), Image.LANCZOS)
    return big


def draw_taps(canvas, tl, inst, tab, sk, t, accent):
    rects = sk.widget_rects(tab)
    over = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    for c in tl.taps:
        age = t - c["t"]
        if c["i"] != inst or not (0 <= age < 0.7) or c["key"] not in rects:
            continue
        x, y, w, h = rects[c["key"]]
        cx = PAGE_X + (x + w / 2) * PAGE_SCALE
        cy = PAGE_Y + (y + h / 2) * PAGE_SCALE
        a = 1.0 - age / 0.7
        r = 18 + 60 * ease(age / 0.7)
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=accent + (int(230 * a),), width=5)
        d.ellipse((cx - 16, cy - 16, cx + 16, cy + 16), fill=(255, 255, 255, int(150 * a)))
        rx0, ry0 = PAGE_X + x * PAGE_SCALE - 4, PAGE_Y + y * PAGE_SCALE - 4
        d.rounded_rectangle((rx0, ry0, rx0 + w * PAGE_SCALE + 8, ry0 + h * PAGE_SCALE + 8), radius=8,
                            outline=accent + (int(255 * a),), width=3)
    canvas.paste(over, (0, 0), over)


def draw_header(d, tl, t, plug, tab, accent, alpha):
    col = mix(BG, accent, alpha)
    text(d, (PAGE_X, 34), plug, 60, col)
    if tab:
        w = font(60).getlength(plug)
        tag = tab + " page"
        tx = PAGE_X + w + 26
        tw = font(24, False).getlength(tag)
        d.rounded_rectangle((tx, 58, tx + tw + 28, 96), radius=19, outline=mix(BG, DIM, alpha), width=2)
        text(d, (tx + 14, 77), tag, 24, mix(BG, DIM, alpha), bold=False, anchor="lm")
    # bar and beat, and the tempo
    beat = t * tl.bpm / 60.0
    bar_no, in_bar = int(beat // 4) + 1, int(beat % 4)
    rx = PAGE_X + PAGE_W
    for k in range(4):
        cx = rx - 18 - (3 - k) * 30
        on = k == in_bar
        frac = (beat % 1.0)
        c = mix(FAINT, accent, (1.0 - 0.6 * frac) if on else 0.0)
        d.ellipse((cx - 9, 68 - 9, cx + 9, 68 + 9), fill=mix(BG, c, alpha))
    text(d, (rx - 140, 68), "BAR %d" % bar_no, 34, mix(BG, INK, alpha), anchor="rm")
    text(d, (rx - 300, 68), "%d BPM" % round(tl.bpm), 24, mix(BG, DIM, alpha), bold=False, anchor="rm")


def wrap(s, px, width, bold):
    words, lines, cur = s.split(), [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if font(px, bold).getlength(trial) > width and cur:
            lines.append(cur)
            cur = w
        else:
            cur = trial
    if cur:
        lines.append(cur)
    return lines


def draw_caption(d, tl, t, accent, width):
    c = tl.last(tl.captions, t)
    sec = tl.section_at(t)
    if not c or (sec and c["t"] < sec["t"]):
        return
    a = ease((t - c["t"]) / 0.2)
    y = PAGE_Y + PAGE_H + 26
    d.rectangle((PAGE_X, y + 4, PAGE_X + 6, y + 92), fill=mix(BG, accent, a))
    text(d, (PAGE_X + 26, y), c["head"], 44, mix(BG, INK, a))
    if c.get("body"):
        for k, line in enumerate(wrap(c["body"], 30, width, False)[:1]):
            text(d, (PAGE_X + 26, y + 56 + 36 * k), line, 30, mix(BG, DIM, a), bold=False)


def draw_crossfader(d, tl, t, accent, alpha):
    st = tl.state["ef"]
    sk = tl.skins["EffectForce"]
    v = st["vals"][sk.index["xfade"]]
    x0, x1, y = PAGE_X + PAGE_W - 470, PAGE_X + PAGE_W - 60, PAGE_Y + PAGE_H + 78
    text(d, ((x0 + x1) / 2, y - 46), "CROSSFADER", 20, mix(BG, DIM, alpha), bold=False, anchor="mm")
    text(d, (x0 - 38, y), "A", 34, mix(BG, INK, alpha), anchor="mm")
    text(d, (x1 + 38, y), "B", 34, mix(BG, INK, alpha), anchor="mm")
    d.rounded_rectangle((x0, y - 5, x1, y + 5), radius=5, fill=mix(BG, FAINT, alpha))
    hx = x0 + v * (x1 - x0)
    d.rounded_rectangle((x0, y - 5, hx, y + 5), radius=5, fill=mix(BG, accent, alpha * 0.8))
    d.rounded_rectangle((hx - 16, y - 26, hx + 16, y + 26), radius=6, fill=mix(BG, (225, 225, 232), alpha),
                        outline=mix(BG, accent, alpha), width=3)


def title_card(t, dur):
    im = Image.new("RGB", (VW, VH), BG)
    d = ImageDraw.Draw(im)
    a = ease(t / 0.8) * (1.0 - ease((t - (dur - 0.45)) / 0.45))
    rise = 24 * (1.0 - ease(t / 0.7))
    text(d, (VW / 2, 420 + rise), "SubForce", 150, mix(BG, ACCENT["SubForce"], a), anchor="mm")
    text(d, (VW / 2, 560), "A melodic-techno build, nothing but SubForce", 56, mix(BG, INK, a), anchor="mm")
    b = a * ease((t - 0.4) / 0.6)
    first = "Each layer comes in clean, then goes through "
    w1, w2 = font(34, False).getlength(first), font(34).getlength("EffectForce")
    x0 = (VW - w1 - w2) / 2
    text(d, (x0, 650), first, 34, mix(BG, DIM, b), bold=False, anchor="lm")
    text(d, (x0 + w1, 650), "EffectForce", 34, mix(BG, ACCENT["EffectForce"], b), anchor="lm")
    text(d, (VW / 2, 790), "An analog-style monosynth that runs inside MPC on the Akai Force", 28, mix(BG, DIM, b * 0.9),
         bold=False, anchor="mm")
    return im


def outro_card(t):
    im = Image.new("RGB", (VW, VH), BG)
    d = ImageDraw.Draw(im)
    a = ease(t / 0.6)
    text(d, (VW / 2, 250), "Free and open source (MIT)", 64, mix(BG, INK, a), anchor="mm")
    rows = [("SubForce", "github.com/Devko/SubForce"), ("EffectForce", "github.com/Devko/EffectForce")]
    for k, (name, url) in enumerate(rows):
        b = a * ease((t - 0.2 - 0.15 * k) / 0.5)
        y = 400 + 90 * k
        text(d, (VW / 2 - 30, y), name, 52, mix(BG, ACCENT[name], b), anchor="rm")
        text(d, (VW / 2 + 10, y), url, 40, mix(BG, INK, b), bold=False, anchor="lm")
    text(d, (VW / 2, 740), "In the MPC VST plugin catalog: sd88me.github.io/mpc-vst-plugins", 30, mix(BG, DIM, a),
         bold=False, anchor="mm")
    text(d, (VW / 2, 790), "Needs root SSH access to the Force (e.g. MockbaMod)  ·  tested on MPC OS 3.9", 30,
         mix(BG, DIM, a), bold=False, anchor="mm")
    text(d, (VW / 2, 960), "Every sound is SubForce, through EffectForce; rendered offline through the plugins' own code, "
         "the pages drawn with the values they reported", 22, mix(BG, FAINT, a * 1.6), bold=False, anchor="mm")
    return im


def draw_mode(d, tl, t, alpha):
    """Clean or through EffectForce (or all together): a pill right of the caption."""
    c = tl.last(tl.modes, t)
    if not c:
        return
    a = alpha * ease((t - c["t"]) / 0.25)
    label, col = {"clean": ("CLEAN", ACCENT["SubForce"]), "fx": ("THROUGH EFFECTFORCE", ACCENT["EffectForce"]),
                  "together": ("ALL TOGETHER", INK)}[c["mode"]]
    w = font(30).getlength(label)
    x1, y = PAGE_X + PAGE_W, PAGE_Y + PAGE_H + 64
    d.rounded_rectangle((x1 - w - 56, y - 30, x1, y + 30), radius=30, outline=mix(BG, col, a), width=3,
                        fill=mix(BG, mix(BG, col, 0.18), a))
    text(d, (x1 - 28, y), label, 30, mix(BG, col, a), anchor="rm")


def render_frame(tl, f, cache):
    t = f / FPS
    tl.advance(f)
    sec = tl.section_at(t)
    name = sec["name"] if sec else "intro"
    if name in ("intro", "outro"):
        if name == "intro":
            nxt = next((s for s in tl.sections if s["t"] > t), None)
            im = title_card(t, nxt["t"] if nxt else 4.0)
        else:
            im = outro_card(t - sec["t"])
        return im
    pg = tl.last(tl.pages, t)
    plug = INSTANCES[pg["i"]] if pg else "SubForce"
    accent = ACCENT[plug]
    im = Image.new("RGB", (VW, VH), BG)
    d = ImageDraw.Draw(im)
    sec_a = ease((t - sec["t"]) / 0.35)
    tab = pg["tab"] if pg else ""
    draw_header(d, tl, t, plug, tab, accent, sec_a)
    if pg:
        page, sk = page_image(tl, pg["i"], pg["tab"], t)
        big = framed_page(page)
        # a page that just came up fades in over the one before
        prev = cache.get("prev")
        age = t - pg["t"]
        if prev is not None and age < 0.25 and cache.get("prev_key") != (pg["i"], pg["tab"]):
            big = Image.blend(prev, big, ease(age / 0.25))
        if "shadow" not in cache:
            sh = Image.new("RGBA", (PAGE_W + 80, PAGE_H + 80), (0, 0, 0, 0))
            ImageDraw.Draw(sh).rounded_rectangle((40, 48, PAGE_W + 40, PAGE_H + 48), radius=14, fill=(0, 0, 0, 160))
            cache["shadow"] = sh.filter(ImageFilter.GaussianBlur(16))
        im.paste(cache["shadow"], (PAGE_X - 40, PAGE_Y - 40), cache["shadow"])
        im.paste(big, (PAGE_X, PAGE_Y))
        d.rounded_rectangle((PAGE_X - 2, PAGE_Y - 2, PAGE_X + PAGE_W + 1, PAGE_Y + PAGE_H + 1), radius=6,
                            outline=mix(BG, accent, 0.35 * sec_a), width=2)
        draw_taps(im, tl, pg["i"], pg["tab"], sk, t, accent)
        if age >= 0.25:
            cache["prev"], cache["prev_key"] = big, (pg["i"], pg["tab"])
    draw_caption(d, tl, t, accent, PAGE_W - 520)
    draw_mode(d, tl, t, sec_a)
    # sections change through the background: the old one fades out, the new one up
    nxt = next((s for s in tl.sections if s["t"] > t + 1e-6), None)
    a = ease((t - sec["t"]) / 0.25)
    if nxt:
        a = min(a, ease((nxt["t"] - t) / 0.2))
    if a < 1.0:
        im = Image.blend(Image.new("RGB", im.size, BG), im, a)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=None)
    ap.add_argument("--name", default="subforce_demo")
    ap.add_argument("--still", type=float, default=None)
    ap.add_argument("--crf", type=int, default=18)
    args = ap.parse_args()
    tl = Timeline()
    cache = {}
    if args.still is not None:
        target = int(round(args.still * FPS))
        for f in range(max(0, target - 12), target):
            render_frame(tl, f, cache)
        render_frame(tl, target, cache).save(os.path.join(OUT, "still.png"))
        return
    end = args.t1 if args.t1 is not None else tl.seconds
    f0, f1 = int(round(args.t0 * FPS)), int(round(end * FPS))
    dur = (f1 - f0) / FPS
    out = os.path.join(OUT, args.name + ".mp4")
    fade = "afade=t=in:st=0:d=0.3," if args.t0 > 0 else ""
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (VW, VH), "-r", str(FPS),
           "-i", "-", "-ss", "%.4f" % args.t0, "-t", "%.4f" % dur, "-i", os.path.join(OUT, "mix.wav"),
           "-af", fade + "afade=t=out:st=%.3f:d=0.6" % max(0.0, dur - 0.6),
           "-c:v", "libx264", "-preset", "slow", "-crf", str(args.crf), "-pix_fmt", "yuv420p", "-tune", "animation",
           "-c:a", "aac", "-b:a", "256k", "-movflags", "+faststart", "-shortest", out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in range(f0 - min(f0, 12), f0):   # warm the state and the page cache
        render_frame(tl, f, cache)
    for f in range(f0, f1):
        im = render_frame(tl, f, cache)
        k = f - f0
        if args.t0 > 0 and k < 9:   # a part fades in from black
            im = Image.blend(Image.new("RGB", im.size, (0, 0, 0)), im, k / 9)
        if f1 - f < 18:
            im = Image.blend(Image.new("RGB", im.size, (0, 0, 0)), im, (f1 - f) / 18)
        p.stdin.write(im.tobytes())
        if k % 150 == 0:
            print("%5.1f s" % (f / FPS), flush=True)
    p.stdin.close()
    p.wait()
    print(out)


if __name__ == "__main__":
    main()
