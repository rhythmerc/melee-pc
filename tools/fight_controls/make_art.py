#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the Fight Controls screen's device pictures (src/pc/fight_menu.cpp)
in Melee's line work, thick gold lines with a glow, into
resources/fight-controls/:

  <device>.png          the device, every part in the normal gold
  <device>-<input>.png  one input's part alone, lit, cut to its glow
  <device>.txt          per input, in the picture's pixels on the screen:
                        "<input> <anchor x> <anchor y> <x> <y> <w> <h>", the
                        anchor where its line ends and where its lit
                        picture sits

<device> is touch or gamepad, <input> the names in src/pc/fight_map.c.
The sources are in source/: Meta's line art of the Touch Plus controllers
(Meta Quest Hardware Art 1.8, Quest_03.ai through pdftocairo -svg; see
meta-lineart-attribution.txt for its terms) and thehoodieguy02's CC0
"8th Gen Console Vector Gamepad Collection" Xbox One pad (OpenGameArt).
Every outline is redrawn at one weight, button glyphs thinner, fills
dropped. Needs Pillow and ImageMagick with librsvg (magick).
Run from anywhere: python3 tools/fight_controls/make_art.py
"""
import os
import re
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "resources", "fight-controls")

SS = 2  # the pictures are drawn at twice the screen's size (1280x720 layout)

# Melee's gold (tools/xr_cards/make_cards.py) and its glow.
GOLD = (240, 194, 38)
GLOW = (255, 150, 20)
LIT = (255, 244, 196)

LINE = 3.0   # outline width, in layout pixels
GLYPH = 1.6  # button letters and icons

# Touch Plus: the parts by path index (pdftocairo's order). Paths 31-41 are
# the white glyph strokes, 30 the Meta button's filled glyph.
TOUCH_PARTS = {
    "left_stick": [6, 7, 8, 9],
    "left_stick_click": [6, 7, 8, 9],
    "left_trigger": [5],
    "left_grip": [1, 2],
    "x": [11, 31, 32],
    "y": [10, 33, 34],
    "menu": [12, 35, 36, 37],
    "right_stick": [13, 14, 15, 16],
    "right_stick_click": [13, 14, 15, 16],
    "right_trigger": [27],
    "right_grip": [23, 24],
    "a": [18, 39, 40, 41],
    "b": [17, 38],
}
TOUCH_GLYPHS = set(range(30, 42))

# The gamepad: the parts by element id. The d-pad is one polygon; each
# direction is the wedge of it on that side.
GAMEPAD_PARTS = {
    "left_stick": "Left_x0020_Stick",
    "left_stick_click": "Left_x0020_Stick",
    "right_stick": "Right_x0020_Stick",
    "right_stick_click": "Right_x0020_Stick",
    "left_trigger": "Left_x0020_Trigger",
    "right_trigger": "Right_x0020_Trigger",
    "left_bumper": "Left_x0020_Shoulder",
    "right_bumper": "Right_x0020_Shoulder",
    "south": "A",
    "east": "B",
    "west": "X",
    "north": "Y",
    "dpad_up": "D-pad",
    "dpad_down": "D-pad",
    "dpad_left": "D-pad",
    "dpad_right": "D-pad",
    "start": "Menu",
}

# The picture's box on the 1280x720 screen: between the two columns of
# input boxes, under the title.
FIT_W, FIT_H = 640, 540


def magick(svg_text, w, h, path):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(svg_text)
        src = f.name
    try:
        subprocess.run(["magick", "-background", "none", "-size", f"{w}x{h}", src, path], check=True)
    finally:
        os.unlink(src)


def render(svg_text, w, h):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.png")
        magick(svg_text, w, h, out)
        return Image.open(out).convert("RGBA").getchannel("A")


class Art:
    """A source SVG cut into elements that can be drawn alone, as lines or filled."""

    def __init__(self, name):
        self.name = name

    def svg(self, keep, mode, w, h):
        """`keep`: element keys drawn (None: all). `mode`: "line", "glyph" or "fill"."""
        raise NotImplementedError


class TouchArt(Art):
    def __init__(self):
        super().__init__("touch")
        s = open(os.path.join(HERE, "source", "touch-plus.svg")).read()
        self.head = s[: s.index("<path")]
        self.paths = re.findall(r"<path [^>]*/>", s)
        vb = re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', self.head)
        self.vw, self.vh = float(vb.group(1)), float(vb.group(2))
        self.parts = TOUCH_PARTS

    def svg(self, keep, mode, w, h):
        scale = w / self.vw
        out = []
        for i, p in enumerate(self.paths):
            if keep is not None and i not in keep:
                continue
            glyph = i in TOUCH_GLYPHS
            if mode == "glyph" and not glyph or mode == "line" and glyph:
                continue
            p = re.sub(r'\s(fill|stroke|stroke-width|stroke-opacity|fill-opacity|stroke-linecap|stroke-linejoin|stroke-miterlimit)="[^"]*"', "", p)
            if mode == "fill":
                style = 'fill="#fff" stroke="#fff" stroke-width="%f"' % (1.0 / scale)
            else:
                width = (GLYPH if glyph else LINE) * SS / scale
                style = 'fill="none" stroke="#fff" stroke-width="%f" stroke-linejoin="round" stroke-linecap="round"' % width
            out.append(p.replace("<path ", "<path " + style + " ", 1))
        head = re.sub(r'width="[^"]*" height="[^"]*"', f'width="{w}" height="{h}"', self.head, count=1)
        return head + "".join(out) + "</svg>"


class GamepadArt(Art):
    def __init__(self):
        super().__init__("gamepad")
        s = open(os.path.join(HERE, "source", "gamepad.svg")).read()
        s = re.sub(r"<\?xml.*?\?>|<!DOCTYPE.*?>|<!--.*?-->", "", s, flags=re.S)
        s = s.replace('<rect class="fil0" width="2048.001" height="2048.001"/>', "")
        s = re.sub(r"<style.*?</style>", "", s, flags=re.S)
        self.src = s
        self.parts = GAMEPAD_PARTS
        # The pad's own extent in the 2048 square, measured, with room for
        # the glow.
        self.crop = (0, 0, 2048, 2048)
        x0, y0, x1, y1 = render(self.svg(None, "line", 1024, 1024), 1024, 1024).getbbox()
        m = 24
        self.crop = (x0 * 2 - m, y0 * 2 - m, x1 * 2 + m, y1 * 2 + m)

    def svg(self, keep, mode, w, h):
        x0, y0, x1, y1 = self.crop
        scale = w / (x1 - x0)
        line = LINE * SS / scale
        glyph = GLYPH * SS / scale
        # Glyphs: the filled letters (fil4-7) and the View/Menu icons (fil0 paths).
        css = ["* { fill: none; stroke: #fff; stroke-width: %f; stroke-linejoin: round; stroke-linecap: round; }" % line,
               ".fil4, .fil5, .fil6, .fil7, path.fil0 { stroke-width: %f; }" % glyph]
        if mode == "line":
            css.append(".fil4, .fil5, .fil6, .fil7, path.fil0 { visibility: hidden; }")
        elif mode == "glyph":
            css.append("* { visibility: hidden; } .fil4, .fil5, .fil6, .fil7, path.fil0 { visibility: visible; }")
        elif mode == "fill":
            css.append("* { fill: #fff; stroke-width: %f; } .fil4, .fil5, .fil6, .fil7, path.fil0 { fill: none; stroke: none; }"
                       % (1.0 / scale))
        if keep is not None:
            ids = ", ".join("#" + k for k in keep)
            sub = ", ".join("#%s *" % k for k in keep)
            css.append("#Layer_x0020_1 * { display: none; }")
            css.append("%s, %s { display: inline; }" % (ids, sub))
        s = re.sub(r'width="2048px" height="2048px"', f'width="{w}" height="{h}"', self.src, count=1)
        s = s.replace('viewBox="0 0 2048 2048"', f'viewBox="{x0} {y0} {x1 - x0} {y1 - y0}"', 1)
        s = s.replace("<defs>", "<defs><style type=\"text/css\"><![CDATA[%s]]></style>" % "\n".join(css), 1)
        return s


def fit(art):
    """The picture's size: fitted into FIT_W x FIT_H, at SS."""
    if isinstance(art, TouchArt):
        aspect = art.vw / art.vh
    else:
        x0, y0, x1, y1 = art.crop
        aspect = (x1 - x0) / (y1 - y0)
    w = FIT_W
    h = int(round(w / aspect))
    if h > FIT_H:
        h = FIT_H
        w = int(round(h * aspect))
    return w * SS, h * SS


def glow_lines(mask, colour, glow, strength=1.0):
    """Gold lines over two blurred copies of themselves: Melee's glowing line work."""
    w, h = mask.size
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for radius, alpha in ((10 * SS, 0.55 * strength), (3.5 * SS, 0.9 * strength)):
        blur = mask.filter(ImageFilter.GaussianBlur(radius))
        blur = blur.point(lambda v, a=alpha: min(255, int(v * a * 2.2)))
        layer = Image.new("RGBA", (w, h), glow + (0,))
        layer.putalpha(blur)
        out.alpha_composite(layer)
    line = Image.new("RGBA", (w, h), colour + (0,))
    line.putalpha(mask)
    out.alpha_composite(line)
    return out


def anchor(mask, name):
    """Where an input's line ends: its part's middle; a d-pad direction's arm."""
    x0, y0, x1, y1 = mask.getbbox()
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    arm = {"dpad_up": (0, -0.36), "dpad_down": (0, 0.36), "dpad_left": (-0.36, 0), "dpad_right": (0.36, 0)}
    if name in arm:
        dx, dy = arm[name]
        cx += dx * (x1 - x0)
        cy += dy * (y1 - y0)
    return cx / SS, cy / SS


def wedge(mask, name):
    """A d-pad direction: the d-pad's quarter on that side (a diagonal cross)."""
    x0, y0, x1, y1 = mask.getbbox()
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = max(x1 - x0, y1 - y0)
    pts = {
        "dpad_up": [(cx, cy), (cx - r, cy - r), (cx + r, cy - r)],
        "dpad_down": [(cx, cy), (cx - r, cy + r), (cx + r, cy + r)],
        "dpad_left": [(cx, cy), (cx - r, cy - r), (cx - r, cy + r)],
        "dpad_right": [(cx, cy), (cx + r, cy - r), (cx + r, cy + r)],
    }[name]
    m = Image.new("L", mask.size, 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return ImageChops.multiply(mask, m)


def keys(art, name):
    part = art.parts[name]
    return set(part) if isinstance(part, list) else {part}


def make(art):
    w, h = fit(art)
    lines = render(art.svg(None, "line", w, h), w, h)
    glyphs = render(art.svg(None, "glyph", w, h), w, h)
    base = glow_lines(ImageChops.lighter(lines, glyphs), GOLD, GLOW)
    base.save(os.path.join(OUT, f"{art.name}.png"), optimize=True)
    anchors = []
    for name in art.parts:
        k = keys(art, name)
        part = ImageChops.lighter(render(art.svg(k, "line", w, h), w, h), render(art.svg(k, "glyph", w, h), w, h))
        fill = render(art.svg(k, "fill", w, h), w, h)
        if name.startswith("dpad_"):
            part, fill = wedge(part, name), wedge(fill, name)
        lit = Image.new("RGBA", (w, h), GOLD + (0,))
        lit.putalpha(fill.point(lambda v: v * 70 // 255))  # the part filled faintly
        # Lit: brighter, a little heavier, a stronger glow.
        heavy = part.filter(ImageFilter.MaxFilter(3))
        lit.alpha_composite(glow_lines(heavy, LIT, GOLD, 1.6))
        # Cut to the glow, on whole screen pixels.
        x0, y0, x1, y1 = lit.getchannel("A").getbbox()
        x0, y0 = x0 // SS * SS, y0 // SS * SS
        x1, y1 = min(w, -(-x1 // SS) * SS), min(h, -(-y1 // SS) * SS)
        lit.crop((x0, y0, x1, y1)).save(os.path.join(OUT, f"{art.name}-{name}.png"), optimize=True)
        ax, ay = anchor(part, name)
        anchors.append(f"{name} {ax:.1f} {ay:.1f} {x0 // SS} {y0 // SS} {(x1 - x0) // SS} {(y1 - y0) // SS}")
    with open(os.path.join(OUT, f"{art.name}.txt"), "w") as f:
        f.write(f"# {art.name}.png at {w // SS}x{h // SS} on the screen; per input: anchor x y, lit picture x y w h\n")
        f.write(f"size {w // SS} {h // SS}\n")
        f.write("\n".join(anchors) + "\n")
    print(f"{art.name}: {w // SS}x{h // SS}, {len(art.parts)} parts")


def main():
    os.makedirs(OUT, exist_ok=True)
    for art in (TouchArt(), GamepadArt()):
        make(art)


if __name__ == "__main__":
    main()
