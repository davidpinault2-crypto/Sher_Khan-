#!/usr/bin/env python3
"""Finish the skin after mpc-vst-plugins has built it (vst.json "skin_post"; run as `skin_post.py <skin dir>`
from mpc/, with Pillow). shadow_skin draws every knob from one filmstrip; the Filterbank 2's knobs are colour coded
by function, so this draws one filmstrip per colour and points each knob at its colour by the parameter it is bound
to. It also prints the page texts (nameplate, preset help) into the backgrounds in Titillium Web, MPC's own font.

The colours follow the hardware's look as reviews describe it (silver-grey panel, colour-coded knobs); which colour
goes with which function on the real panel isn't documented anywhere we could read, so KNOB_COLOURS is a best
guess. Change it here: one line per group."""
import glob
import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

KNOB_COLOURS = {   # cap colour, keys
    "red": ((198, 40, 32), ["freq_1", "reso_1", "adsr_1", "lbh_1", "corr_1"]),
    "blue": ((38, 90, 168), ["freq_2", "reso_2", "adsr_2", "lbh_2", "corr_2", "harmonics"]),
    "yellow": ((224, 176, 32), ["attack", "decay", "sustain", "release"]),
    "green": ((50, 138, 72), ["fm", "lfo_speed", "lfo_depth", "am"]),
    "black": ((34, 35, 38), ["input", "par_ser", "byp_eff", "output", "grit"]),
}
FRAMES = 128          # stock knob strips: 128 frames, numFrames 127
INK = (23, 24, 26)
INK_DIM = (60, 63, 66)
Y_OFF = 86            # layout y of the plugin area's top edge

# Text printed into each tab's background: (tab, text, layout x, layout y, size px, colour, weight)
TEXTS = [
    (1, "DUAL MULTIMODE", 1113, 520, 30, INK, "SemiBold"),
    (1, "FILTER BANK", 1113, 556, 30, INK, "SemiBold"),
    (1, "model of the Sherman Filterbank 2", 1113, 612, 19, INK_DIM, "Regular"),
    (1, "v0.2.0", 1113, 646, 19, INK_DIM, "Regular"),
    (2, "Arrows or Q-Link 1 pick a preset; LOAD recalls it (so does MPC's PRESET menu).", 640, 590, 22, INK_DIM, "Regular"),
    (2, "SAVE stores the current sound in the selected User slot.", 640, 626, 22, INK_DIM, "Regular"),
    (2, "Every setting is also saved with the project.", 640, 662, 22, INK_DIM, "Regular"),
]


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def knob_frame_base(size, ss, cap):
    """The parts of a knob that don't turn: shadow, scale ticks, skirt, cap (supersampled by ss)."""
    s = size * ss
    c = s / 2.0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    r_skirt = 39 * ss
    ImageDraw.Draw(shadow).ellipse([c - r_skirt + 2 * ss, c - r_skirt + 4 * ss, c + r_skirt + 2 * ss, c + r_skirt + 4 * ss],
                                   fill=(0, 0, 0, 120))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(4 * ss)))
    d = ImageDraw.Draw(img)
    for t in range(11):   # printed scale: 11 ticks over the 270 degree travel
        a = math.radians(-135 + 27 * t)
        r0, r1 = 44 * ss, 49.5 * ss
        d.line([c + r0 * math.sin(a), c - r0 * math.cos(a), c + r1 * math.sin(a), c - r1 * math.cos(a)],
               fill=INK + (255,), width=int(2.2 * ss))
    # skirt: dark ring with a soft top light
    for i in range(r_skirt, 0, -ss):
        t = i / r_skirt
        col = mix((70, 72, 76), (28, 29, 32), t ** 0.5)
        d.ellipse([c - i, c - i - (1 - t) * 2 * ss, c + i, c + i - (1 - t) * 2 * ss], fill=col + (255,))
    # cap: the function colour, lit from the top left
    r_cap = 30 * ss
    hi = mix(cap, (255, 255, 255), 0.45)
    lo = mix(cap, (0, 0, 0), 0.35)
    d.ellipse([c - r_cap - ss, c - r_cap - ss, c + r_cap + ss, c + r_cap + ss], fill=(15, 15, 17, 255))
    for i in range(r_cap, 0, -ss):
        t = i / r_cap
        col = mix(hi, cap, min(1.0, t * 1.3)) if t < 0.75 else mix(cap, lo, (t - 0.75) / 0.25)
        off = (1 - t) * 6 * ss
        d.ellipse([c - i - off, c - i - off, c + i - off, c + i - off], fill=col + (255,))
    return img


def knob_strip(path, cap, size):
    ss = 4
    base = knob_frame_base(size, ss, cap)
    s = size * ss
    c = s / 2.0
    strip = Image.new("RGBA", (size, size * FRAMES), (0, 0, 0, 0))
    pointer = (250, 250, 250) if sum(cap) < 600 else INK
    for k in range(FRAMES):
        f = base.copy()
        d = ImageDraw.Draw(f)
        a = math.radians(-135 + 270.0 * k / (FRAMES - 1))
        r0, r1 = 7 * ss, 37 * ss
        p0 = (c + r0 * math.sin(a), c - r0 * math.cos(a))
        p1 = (c + r1 * math.sin(a), c - r1 * math.cos(a))
        d.line([p0, p1], fill=(10, 10, 12, 255), width=int(7 * ss))
        d.line([p0, p1], fill=pointer + (255,), width=int(4 * ss))
        strip.paste(f.resize((size, size), Image.LANCZOS), (0, k * size))
    strip.save(path, optimize=True)


def main():
    skin_dir = sys.argv[1]
    skins = os.path.join(skin_dir, "Plugin Skins")
    params = [p["key"] for p in json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "params.json")))["params"]]
    colour_of = {k: name for name, (_, keys) in KNOB_COLOURS.items() for k in keys}
    tui_path = os.path.join(skins, "TUI.json")
    tui = json.load(open(tui_path))
    defs = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
    by_key = {d["key"]: d for d in defs}

    made, old_strips, retargeted = {}, set(), 0
    pages = [d for d in defs if "componentsData" in d["value"] and "|" in d["key"]]
    for page in pages:
        for comp in page["value"]["componentsData"]:
            ctype = comp["componentData"]["type"]
            if not ctype.startswith("shKnob"):
                continue
            ref = [m["value"] for m in comp["handle remapping"]["map"] if m["key"] == "Data"]
            index = int(ref[0].split()[-1]) if ref else -1
            key = params[index] if 0 <= index < len(params) else None
            colour = colour_of.get(key)
            if not colour:
                sys.exit("skin_post: knob for %r has no colour in KNOB_COLOURS" % key)
            new_key = "skKnob_%s__%s" % (colour, ctype)
            if new_key not in by_key:
                src = by_key[ctype]
                clone = json.loads(json.dumps(src))
                clone["key"] = new_key
                for sub in clone["value"]["componentsData"]:
                    data = sub["componentData"].get("data", {})
                    if data.get("knobType") == "FilmStrip":
                        old_strips.add(data["filmStrip"])
                        w = int(sub["bounds"]["bounds"].split()[2])
                        strip = "sk_knob_%s_%d.png" % (colour, w)
                        if strip not in made:
                            knob_strip(os.path.join(skins, strip), KNOB_COLOURS[colour][0], w)
                            made[strip] = w
                        data["filmStrip"] = strip
                defs.append(clone)
                by_key[new_key] = clone
            comp["componentData"]["type"] = new_key
            retargeted += 1

    # the original single-colour knob definitions and strips are no longer used
    used = {comp["componentData"]["type"] for page in pages for comp in page["value"]["componentsData"]}
    defs[:] = [d for d in defs if not (d["key"].startswith("shKnob") and d["key"] not in used)]
    still = json.dumps(tui)
    for s in old_strips:
        if s not in still and os.path.exists(os.path.join(skins, s)):
            os.remove(os.path.join(skins, s))
    json.dump(tui, open(tui_path, "w"), indent=4)

    # page texts, in MPC's own typeface (the font the build uses for frame titles)
    title_font = os.environ.get("SHADOW_TITLE_FONT")
    if title_font:
        for tab, text, x, y, size, colour, weight in TEXTS:
            bg = os.path.join(skins, "sh_bg_%d.png" % tab)
            if not os.path.exists(bg):
                continue
            path = title_font.replace("SemiBold", weight)
            font = ImageFont.truetype(path if os.path.exists(path) else title_font, size)
            img = Image.open(bg).convert("RGB")
            ImageDraw.Draw(img).text((x, y - Y_OFF), text, font=font, fill=colour, anchor="mm")
            img.save(bg)
    print("skin_post: %d knobs colour-coded (%s), %d page texts" %
          (retargeted, ", ".join(sorted(made)), len(TEXTS) if title_font else 0))


if __name__ == "__main__":
    main()
