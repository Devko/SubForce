"""Draws a plugin page the way MPC draws it, from the plugin's built skin and a parameter state.

The skin's images (backgrounds, knob and slider filmstrips, tiles, segments, pills, buttons) are
placed where shadow_skin.py's build() puts their components in TUI.json, and the live text MPC
draws (names, values, readouts, list rows) is set in Titillium Web at the same boxes, sizes and
colours. Geometry comes from each repo's own vendored shadow_skin.py, so the pages follow the
layouts as they are.

    skin = Skin(r"D:/DEV/EffectForce")
    img = skin.page("PERFORM", values, texts)   # 1280x628 RGB
"""
import importlib.util
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, Y_OFF = 1280, 628, 86
_FONT_EM = None   # (ascent + descent) per px of PIL size, for JUCE's font heights


def _load_shadow_skin(repo, tag):
    tools = os.path.join(repo, "third_party", "mpc-vst-plugins", "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    spec = importlib.util.spec_from_file_location("shadow_skin_" + tag, os.path.join(tools, "shadow_skin.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Skin:
    def __init__(self, repo):
        self.repo = repo
        self.tag = os.path.basename(repo.rstrip("/\\"))
        self.ss = _load_shadow_skin(repo, self.tag)
        layout = os.path.join(repo, "surface", "layout.conf")
        self.tabs, top = self.ss.parse_layout(layout)
        self.ss.apply_theme(top)
        if self.ss.FONT_LABEL_PATH and not os.path.isabs(self.ss.FONT_LABEL_PATH):   # relative to surface/, as the build runs
            self.ss.FONT_LABEL_PATH = os.path.join(repo, "surface", self.ss.FONT_LABEL_PATH)
        for tab in self.tabs:
            tab["widgets"] = self.ss.expand_pictures(tab["widgets"])
        self.params = json.load(open(os.path.join(repo, "surface", "params.json"), encoding="utf-8"))["params"]
        self.index = {p["key"]: i for i, p in enumerate(self.params)}
        skin_root = os.path.join(repo, "surface", "build", "skin")
        vendor = next(d for d in os.listdir(skin_root) if os.path.isdir(os.path.join(skin_root, d)))
        self.dir = os.path.join(skin_root, vendor, "Plugin Skins")
        self.font_path = os.path.join(repo, "surface", "fonts", "TitilliumWeb-SemiBold.ttf")
        self._img, self._strip, self._font = {}, {}, {}
        ss = self.ss
        self.ink, self.ink_dim, self.accent = "#" + ss.INK, "#" + ss.INK_DIM, "#" + ss.ACCENT
        self.display_ink = "#" + ss.DISPLAY_INK
        self.label_scale = ss.LABEL_SCALE
        # Option lists for enums and popups, as build() fills them in.
        for tab in self.tabs:
            for w in tab["widgets"]:
                if w["kind"] in ("enum_h", "enum_v", "popup") and not w.get("options") and w.get("key") in self.index:
                    opts = [str(o) for o in self.params[self.index[w["key"]]].get("options") or []]
                    w["options"] = [o.upper() for o in opts] if w["kind"].startswith("enum") else opts

    # --- assets -----------------------------------------------------------------------------
    def img(self, name):
        if name not in self._img:
            path = os.path.join(self.dir, name)
            self._img[name] = Image.open(path).convert("RGBA") if os.path.exists(path) else None
        return self._img[name]

    def frame(self, name, value):
        """A filmstrip's frame for a 0..1 value (square frames, stacked vertically)."""
        if name not in self._strip:
            im = self.img(name)
            if im is None:
                self._strip[name] = None
            else:
                side = im.size[0]
                n = im.size[1] // side
                self._strip[name] = [im.crop((0, k * side, side, (k + 1) * side)) for k in range(n)]
        frames = self._strip[name]
        if not frames:
            return None
        k = int(round(max(0.0, min(1.0, value)) * (len(frames) - 1)))
        return frames[k]

    def font(self, height):
        """JUCE's Font(height): ascent + descent = height."""
        global _FONT_EM
        if _FONT_EM is None:
            a, d = ImageFont.truetype(self.font_path, 100).getmetrics()
            _FONT_EM = (a + d) / 100.0
        key = round(height, 2)
        if key not in self._font:
            self._font[key] = ImageFont.truetype(self.font_path, max(6, int(round(height / _FONT_EM))))
        return self._font[key]

    @staticmethod
    def put(im, part, xy):
        if part is not None:
            im.paste(part, (int(xy[0]), int(xy[1])), part)

    # --- text, as a JUCE Label: fitted (squeezed to 0.7, then cut with an ellipsis) ------------
    def label(self, im, box, text, height, colour, align="center", upper=False):
        if not text:
            return
        if upper:
            text = text.upper()
        x, y, w, h = box
        f = self.font(height)
        a, d = f.getmetrics()
        tw = f.getlength(text)
        if tw > w * 1.0 / 0.7:
            while len(text) > 1 and f.getlength(text + "...") > w / 0.7:
                text = text[:-1]
            text = text.rstrip() + "..."
            tw = f.getlength(text)
        scale = min(1.0, w / tw) if tw > 0 else 1.0
        ty = y + (h - (a + d)) / 2.0
        if scale >= 0.999:
            tx = x + (w - tw) / 2.0 if align == "center" else x
            ImageDraw.Draw(im).text((tx, ty), text, font=f, fill=colour)
            return
        # squeezed: draw wide, then scale horizontally
        tmp = Image.new("L", (int(tw) + 4, a + d + 2), 0)
        ImageDraw.Draw(tmp).text((2, 0), text, font=f, fill=255)
        nw = max(1, int(tmp.size[0] * scale))
        tmp = tmp.resize((nw, tmp.size[1]), Image.LANCZOS)
        tx = x + (w - nw) / 2.0 if align == "center" else x
        im.paste(Image.new("RGB", tmp.size, colour), (int(tx), int(ty)), tmp)

    def tab_index(self, name):
        for t, tab in enumerate(self.tabs):
            if tab["name"] == name:
                return t
        raise KeyError("%s has no tab %s" % (self.tag, name))

    def option_of(self, key, values):
        p = self.params[self.index[key]]
        n = len(p.get("options") or [])
        v = values[self.index[key]]
        return int(round(v * (n - 1))) if n > 1 else 0

    def _shown(self, w, values):
        if not w.get("when"):
            return True
        k, _, o = w["when"].partition(":")
        opts = [str(x).lower() for x in (self.params[self.index[k]].get("options") or [])]
        oi = opts.index(o.lower()) if o.lower() in opts else int(o)
        return self.option_of(k, values) == oi

    def widget_rects(self, tab_name):
        """{key: (x, y, w, h) in plugin pixels} for every control on a tab (list rows by row key)."""
        ss, out = self.ss, {}
        for w in self.tabs[self.tab_index(tab_name)]["widgets"]:
            k = w["kind"]
            if k == "list":
                for (x, y, tw, th), key in zip(ss.list_tiles(w), ss.list_keys(w)):
                    out[key] = (x, y - Y_OFF, tw, th)
            elif k == "button":
                x, y, bw, bh = ss.button_rect(w)
                out[w["key"]] = (x, y - Y_OFF, bw, bh)
            elif k == "knob":
                s = 2 * w["r"] + 10
                out[w["key"]] = (w["cx"] - s // 2, w["cy"] - s // 2 - Y_OFF, s, s)
            elif k in ("readout", "popup", "stepper", "menu", "slider_v", "slider_h", "meter"):
                out[w["key"]] = (w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2 - Y_OFF, w["w"], w["h"])
            elif k in ("enum_h", "enum_v") and w.get("options"):
                rs = ss.seg_rects(w)
                x0, y0 = min(r[0] for r in rs), min(r[1] for r in rs)
                x1, y1 = max(r[0] + r[2] for r in rs), max(r[1] + r[3] for r in rs)
                out[w["key"]] = (x0, y0 - Y_OFF, x1 - x0, y1 - y0)
            elif k == "toggle":
                out[w["key"]] = (w["cx"] - 30, w["cy"] - 18 - Y_OFF, 60, 36)
        return out

    # --- the page ---------------------------------------------------------------------------
    def page(self, tab_name, values, texts, pressed=()):
        ss = self.ss
        t = self.tab_index(tab_name)
        tab = self.tabs[t]
        im = self.img("sh_bg_%d.png" % t).convert("RGB")

        # when= widgets' baked parts: one image per mode, as build() makes them
        modes = []
        for w in tab["widgets"]:
            if w.get("when") and w["when"] not in [m[0] for m in modes]:
                modes.append((w["when"], [x for x in tab["widgets"] if x.get("when") == w["when"]]))
        base = [w for w in tab["widgets"] if not w.get("when")]
        m_i = 0
        for when, ws in modes:
            rects = [ss.baked_rect(w) for w in ws if ss.baked_rect(w)]
            if not rects:
                continue
            if self._shown(ws[0], values):
                bx, by = max(0, min(r[0] for r in rects)), max(Y_OFF, min(r[1] for r in rects))
                mi = self.img("sh_mode_%d_%d.png" % (t, m_i))
                self.put(im, mi, (bx, by - Y_OFF))
            m_i += 1

        ls = self.label_scale
        for w in tab["widgets"]:
            kind = w["kind"]
            if kind not in ss.CONTROL_KINDS or not self._shown(w, values):
                continue
            key = w.get("key")
            i = self.index.get(key, -1)
            v = values[i] if i >= 0 else 0.0
            txt = texts[i] if i >= 0 else ""
            if kind == "knob":
                r = w["r"]
                s, cw = 2 * r + 10, max(130, 2 * r + 10)
                name_h = round(20 * ls)
                name_y = s // 2 + r + 2
                value_y = name_y + name_h + 2
                value_h = round(26 * ls)
                x0, y0 = w["cx"] - cw // 2, w["cy"] - s // 2 - Y_OFF
                fr = self.frame("sh_knob_r%d.png" % r, v)
                self.put(im, fr, (w["cx"] - s // 2, y0))
                self.label(im, (x0, y0 + name_y, cw, name_h), self.params[i]["name"], 17.0 * ls, self.ink)
                self.label(im, (x0, y0 + value_y, cw, value_h), txt, 22.0 * ls, self.ink_dim, upper=True)
            elif kind == "toggle":
                x0, y0 = w["cx"] - 60, w["cy"] - 18 - Y_OFF
                pill = self.img("sh_pill_%s.png" % ("on" if v >= 0.5 else "off"))
                self.put(im, pill, (x0 + 33, y0 + 4))
                self.label(im, (x0, y0 + 34, 120, 20), self.params[i]["name"], 15.0, self.ink)
            elif kind == "button":
                x, y, bw, bh = ss.button_rect(w)
                state = "on" if key in pressed else "off"
                b = self.img("sh_btn_%s_%s_%s.png" % (key, ss.slug(w.get("label", "")), state))
                self.put(im, b, (x, y - Y_OFF))
            elif kind in ("slider_v", "slider_h", "meter"):
                sq = max(w["w"], w["h"])
                img = "sh_%s_%dx%d.png" % ("meter" if kind == "meter" else kind, w["w"], w["h"])
                fr = self.frame(img, v)
                y0 = w["cy"] - sq // 2 - Y_OFF
                self.put(im, fr, (w["cx"] - sq // 2, y0))
                if kind != "meter":
                    cw = w.get("cw", max(130, sq))
                    name_y = (sq - w["h"]) // 2 + w["h"] + 2
                    self.label(im, (w["cx"] - cw // 2, y0 + name_y, cw, 20), self.params[i]["name"], 17.0, self.ink)
                    self.label(im, (w["cx"] - cw // 2, y0 + name_y + 22, cw, 26), txt, 22.0, self.ink_dim)
            elif kind == "menu":
                x, y = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2 - Y_OFF
                self.label(im, (x + 8, y, w["w"] - 16, w["h"]), txt, 26.0, self.accent)
            elif kind == "popup":
                x, y = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2 - Y_OFF
                self.label(im, (x + 8, y, w["w"] - 44, w["h"]), txt, 26.0, self.accent)
            elif kind == "readout":
                x, y = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2 - Y_OFF
                ink = self.display_ink if w.get("style") == "dotmatrix" else self.accent
                self.label(im, (x + 8, y, w["w"] - 16, w["h"]), txt, 26.0, ink)
            elif kind == "stepper":
                x0, y0, h = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2 - Y_OFF, w["h"]
                gi = self.index.get(w.get("get"), i) if w.get("get") else i
                ink = self.display_ink if w.get("style") == "dotmatrix" else self.accent
                self.label(im, (x0 + h + 3 + 8, y0, w["w"] - 2 * h - 22, h), texts[gi], 26.0, ink)
            elif kind == "list":
                for (x, y, tw, th), sk in zip(ss.list_tiles(w), ss.list_keys(w)):
                    si = self.index[sk]
                    tile = self.img("sh_tile_%dx%d_%s.png" % (tw, th, "on" if values[si] >= 0.5 else "off"))
                    self.put(im, tile, (x, y - Y_OFF))
                    self.label(im, (x + 12, y - Y_OFF, tw - 24, th), texts[si], 24.0, self.accent, align="left")
            elif kind in ("enum_h", "enum_v"):
                n = len(w["options"])
                sel = int(round(v * (n - 1))) if n > 1 else 0
                for o, (x, y, sw, sh) in enumerate(ss.seg_rects(w)):
                    seg = self.img("sh_seg_%s_%d_%s.png" % (key, o, "on" if o == sel else "off"))
                    self.put(im, seg, (x, y - Y_OFF))
        return im
