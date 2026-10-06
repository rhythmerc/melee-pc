#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the XR stage-placing pictures (src/pc/xr_place.c) in Melee's menu
style, into resources/xr/:

  place-cards.png, place-legend.png              for Touch controllers
  place-cards-hands.png, place-legend-hands.png  for tracked hands
  place-clip-<set>-<name>.jpg, place-clips.txt   their looping clips

The gesture cards play clips cut from headset recordings made with the
xr-recording branch (chroma green backdrop, ghost hands and controllers):
footage/<set>-<name>.mp4, trimmed and cropped, <set> being controllers or
hands. Each is keyed, composited over the cards' grid backdrop, played
forward then back so it loops, and packed into a frame atlas;
place-clips.txt says where each sits on its cards. The Ready card's still
is a left-eye capture of Battlefield from the desktop XR build in shots/
(AURORA_XR_DUMP; premultiplied colour, alpha = coverage), as are the
fallbacks drawn when a clip's footage is missing. Needs Pillow and ffmpeg.
Run from anywhere: python3 tools/xr_cards/make_cards.py
"""
import math
import os
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "resources", "xr")
FONT = os.path.join(ROOT, "resources", "font-bold.ttf")

SS = 2  # supersampling: drawn at twice the size, then reduced

# Melee's menu palette (VS. Mode, the pause screen).
FRAME_RED = (222, 70, 38)
FRAME_DARK = (120, 30, 18)
GOLD = (240, 194, 38)
GOLD_DARK = (150, 110, 10)
PANEL = (10, 10, 14)
SILVER = (205, 208, 216)
GRID_BG = (22, 24, 48)
GRID_LINE = (52, 62, 120)
LASER = (115, 217, 255)
WHITE = (250, 250, 250)

BUTTONS = {
    # label: (fill, text colour, shape)
    "A": ((0, 168, 96), WHITE, "circle"),
    "B": ((214, 40, 40), WHITE, "circle"),
    "Y": ((214, 214, 220), (30, 30, 30), "pill"),
    "START": ((150, 152, 160), WHITE, "pill"),
    "GRIP": ((60, 64, 76), WHITE, "pill"),
    "PINCH": ((60, 64, 76), WHITE, "pill"),
}


def font(size):
    return ImageFont.truetype(FONT, size * SS)


def s(v):
    return int(round(v * SS))


def italic_text(img, xy, text, size, fill, outline=None, shear=0.22, anchor="ls"):
    """Melee's slanted headings: text drawn upright, then sheared."""
    f = font(size)
    l, t, r, b = f.getbbox(text, anchor=anchor)
    pad = s(size)
    w, h = r - l + 2 * pad, b - t + 2 * pad
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    origin = (pad - l, pad - t)
    if outline:
        d.text(origin, text, font=f, fill=outline, anchor=anchor, stroke_width=s(3), stroke_fill=outline)
    d.text(origin, text, font=f, fill=fill, anchor=anchor)
    # x' = x + shear * (y - baseline): lean right about the baseline.
    base = origin[1]
    layer = layer.transform(layer.size, Image.AFFINE, (1, shear, -shear * base, 0, 1, 0), Image.BICUBIC)
    img.alpha_composite(layer, (int(xy[0] - origin[0]), int(xy[1] - origin[1])))


def button(d, x, y, label, size=34):
    """A GameCube button glyph, its left edge at x, centred on y."""
    fill, ink, shape = BUTTONS[label]
    f = font(size * (0.62 if shape == "circle" else 0.5))
    if shape == "circle":
        r = s(size) // 2
        d.ellipse((x, y - r, x + 2 * r, y + r), fill=fill, outline=(0, 0, 0), width=s(2))
        d.text((x + r, y), label, font=f, fill=ink, anchor="mm")
        return x + 2 * r
    tw = f.getlength(label)
    h = s(size * 0.78)
    w = int(tw + h * 0.9)
    d.rounded_rectangle((x, y - h // 2, x + w, y + h // 2), radius=h // 2, fill=fill, outline=(0, 0, 0), width=s(2))
    d.text((x + w // 2, y), label, font=f, fill=ink, anchor="mm")
    return x + w


def rich_line(d, x, y, parts, size, fill=WHITE):
    """A line of text with button glyphs: parts are strings or ("btn", label)."""
    f = font(size)
    for p in parts:
        if isinstance(p, tuple):
            x = button(d, x, y, p[1], size=size * 1.25) + s(8)
        else:
            d.text((x, y), p, font=f, fill=fill, anchor="lm")
            x += int(f.getlength(p))
    return x


def rich_width(parts, size):
    probe = Image.new("RGBA", (s(2000), s(200)))
    return rich_line(ImageDraw.Draw(probe), 0, s(100), parts, size)


def grid_backdrop(w, h):
    """Melee's menu backdrop: a blue grid on deep blue."""
    bg = Image.new("RGBA", (w, h), GRID_BG + (255,))
    d = ImageDraw.Draw(bg)
    step = s(34)
    for x in range(0, w, step):
        d.line((x, 0, x, h), fill=GRID_LINE, width=s(1))
    for y in range(0, h, step):
        d.line((0, y, w, y), fill=GRID_LINE, width=s(1))
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((-w * 0.2, h * 0.15, w * 1.2, h * 1.6), fill=(70, 40, 120, 90))
    bg.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s(40))))
    return bg


def shot(name, w, h):
    """A screenshot over the grid backdrop, fitted into w x h."""
    src = Image.open(os.path.join(HERE, "shots", name + ".png")).convert("RGBA")
    k = min(w / src.width, h / src.height) * 0.98
    src = src.resize((int(src.width * k), int(src.height * k)), Image.LANCZOS)
    bg = grid_backdrop(w, h)
    # Premultiplied capture: out = colour + backdrop * (1 - alpha).
    ox, oy = (w - src.width) // 2, (h - src.height) // 2
    region = bg.crop((ox, oy, ox + src.width, oy + src.height))
    rp, sp = region.load(), src.load()
    for y in range(src.height):
        for x in range(src.width):
            r, g, b, a = sp[x, y]
            br, bgc, bb, _ = rp[x, y]
            k1 = 1 - a / 255
            rp[x, y] = (min(255, int(r + br * k1)), min(255, int(g + bgc * k1)), min(255, int(b + bb * k1)), 255)
    bg.paste(region, (ox, oy))
    return bg


def laser(img, start, end):
    """A pointing laser like the headset's: a cyan beam with a dot."""
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    for width, alpha in ((s(22), 50), (s(12), 110), (s(5), 255)):
        d.line((start, end), fill=LASER + (alpha,), width=width)
    r = s(13)
    d.ellipse((end[0] - r, end[1] - r, end[0] + r, end[1] + r), fill=LASER + (255,))
    r = s(6)
    d.ellipse((end[0] - r, end[1] - r, end[0] + r, end[1] + r), fill=WHITE + (255,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(s(1.2))))


def arrow(img, start, end, width=12, head=30):
    """A bold white arrow with a dark outline."""
    d = ImageDraw.Draw(img)
    ang = math.atan2(end[1] - start[1], end[0] - start[0])
    hx, hy = end[0] - math.cos(ang) * s(head) * 0.8, end[1] - math.sin(ang) * s(head) * 0.8
    tip = [
        end,
        (end[0] - math.cos(ang - 0.5) * s(head), end[1] - math.sin(ang - 0.5) * s(head)),
        (end[0] - math.cos(ang + 0.5) * s(head), end[1] - math.sin(ang + 0.5) * s(head)),
    ]
    d.line((start, (hx, hy)), fill=(0, 0, 0), width=s(width + 8))
    d.polygon(tip, fill=(0, 0, 0), outline=(0, 0, 0), width=s(8))
    d.line((start, (hx, hy)), fill=WHITE, width=s(width))
    d.polygon(tip, fill=WHITE)


def turn_arrow(img, center, radius, a0, a1, width=12):
    """A curved arrow about `center` from angle a0 to a1 (degrees)."""
    d = ImageDraw.Draw(img)
    cx, cy = center
    box = (cx - radius, cy - radius * 0.38, cx + radius, cy + radius * 0.38)
    d.arc(box, a0, a1 - 6, fill=(0, 0, 0), width=s(width + 8))
    d.arc(box, a0, a1 - 6, fill=WHITE, width=s(width))
    t = math.radians(a1)
    end = (cx + radius * math.cos(t), cy + radius * 0.38 * math.sin(t))
    t0 = math.radians(a1 - 12)
    before = (cx + radius * math.cos(t0), cy + radius * 0.38 * math.sin(t0))
    arrow(img, before, end, width=width, head=30)


def card(w, h, title, picture, lines):
    """One how-to card: gold rim, gold title pill, picture, caption.
    Returns it and the picture's rectangle on it (x, y, w, h)."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    rim = s(6)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=s(26), fill=PANEL + (238,), outline=GOLD, width=rim)
    # The title: a gold pill with black text, like Melee's selected item.
    pill_h = s(56)
    d.rounded_rectangle((s(16), s(16), w - s(16), s(16) + pill_h), radius=pill_h // 2, fill=GOLD)
    d.rounded_rectangle((s(16), s(16) + pill_h - s(10), w - s(16), s(16) + pill_h), radius=s(5), fill=GOLD_DARK)
    d.rounded_rectangle((s(16), s(16), w - s(16), s(16) + pill_h - s(6)), radius=pill_h // 2, fill=GOLD)
    italic_text(img, (w // 2 - font(32).getlength(title) // 2, s(16) + pill_h * 0.72), title, 32, (16, 12, 4))
    # The picture.
    px, py = s(20), s(16) + pill_h + s(14)
    pw, ph = w - 2 * px, int((w - 2 * px) * 0.7)
    pic = picture(pw, ph)
    mask = Image.new("L", (pw, ph), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, pw - 1, ph - 1), radius=s(14), fill=255)
    img.paste(pic, (px, py), mask)
    d.rounded_rectangle((px, py, px + pw - 1, py + ph - 1), radius=s(14), outline=(70, 70, 80), width=s(2))
    # The caption.
    y = py + ph + s(40)
    for parts in lines:
        rich_line(d, px + s(4), y, parts, 26)
        y += s(40)
    return img, (px, py, pw, ph)


# The board's layout (supersampled pixels): four cards in a row.
BOARD_W, BOARD_H = s(1680), s(640)
BOARD_TOP = s(64)
CARD_W, CARD_H = s(385), s(526)
CARD_GAP = s(20)
CARD_Y = BOARD_TOP + s(30)


def card_x(i):
    return s(40) + i * (CARD_W + CARD_GAP)


def picture_size():
    pw = CARD_W - 2 * s(20)
    return pw, int(pw * 0.7)


def picture_move(w, h):
    img = shot("move", w, h)
    hit = (int(w * 0.47), int(h * 0.66))
    laser(img, (int(w * 1.02), int(h * 1.05)), hit)
    arrow(img, (int(w * 0.40), int(h * 0.42)), (int(w * 0.16), int(h * 0.30)))
    return img


def picture_turn(w, h):
    img = shot("turn", w, h)
    laser(img, (int(-w * 0.02), int(h * 1.05)), (int(w * 0.26), int(h * 0.70)))
    laser(img, (int(w * 1.02), int(h * 1.05)), (int(w * 0.80), int(h * 0.74)))
    turn_arrow(img, (w // 2, int(h * 0.22)), int(w * 0.3), 200, 340)
    return img


def picture_scale(w, h):
    img = shot("turn", w, h)
    laser(img, (int(-w * 0.02), int(h * 1.05)), (int(w * 0.26), int(h * 0.70)))
    laser(img, (int(w * 1.02), int(h * 1.05)), (int(w * 0.80), int(h * 0.74)))
    arrow(img, (int(w * 0.30), int(h * 0.50)), (int(w * 0.08), int(h * 0.38)))
    arrow(img, (int(w * 0.72), int(h * 0.52)), (int(w * 0.94), int(h * 0.40)))
    return img


def picture_start(w, h):
    img = shot("front", w, h)
    d = ImageDraw.Draw(img)
    # A big A, as the pause screen shows its buttons.
    r = s(40)
    cx, cy = int(w * 0.82), int(h * 0.24)
    d.ellipse((cx - r - s(4), cy - r - s(4), cx + r + s(4), cy + r + s(4)), fill=(0, 0, 0))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=BUTTONS["A"][0])
    d.text((cx, cy), "A", font=font(46), fill=WHITE, anchor="mm")
    return img


# ---------------------------------------------------------------- clips

CLIPS = ["move", "turn", "scale"]  # the hand cards' first three, in order
CLIP_FPS = 12


HUD_BAND = 280  # source rows that can hold the HUD, above the stage


def strip_hud(im):
    """Paints the HUD's leftovers green, before keying: the hand takes were
    recorded before the xr-recording branch hid the HUD for every hold, so
    the stock icons and bits of "Ready" float above the stage. In the top
    band they're the small, roundish or tiny blobs; the stage's platforms
    there are wide and thin, and lasers and hands join larger shapes."""
    w, h = im.width, min(im.height, HUD_BAND)
    px = im.load()
    fg = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            fg[y * w + x] = g - max(r, b) < 190  # coverage over about 0.25
    seen = bytearray(w * h)
    for start in range(w * h):
        if not fg[start] or seen[start]:
            continue
        seen[start] = 1
        stack, blob = [start], []
        while stack:
            i = stack.pop()
            blob.append(i)
            x, y = i % w, i // w
            for j in (i - 1 if x else -1, i + 1 if x < w - 1 else -1, i - w, i + w):
                if 0 <= j < w * h and fg[j] and not seen[j]:
                    seen[j] = 1
                    stack.append(j)
        xs = [i % w for i in blob]
        ys = [i // w for i in blob]
        bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        if max(ys) >= h - 1:
            continue  # runs on below the band: part of the stage or a hand
        if len(blob) < 60 or (bw <= 80 and bh <= 80 and bh >= 0.45 * bw):
            for i in blob:
                px[i % w, i // w] = (0, 255, 0)
    return im


def keyed_frames(clipset, name, w, h):
    """The clip's frames (1x pixels), keyed and over the grid backdrop.

    The take is lit against pure green (0, 255, 0) and nothing in it is
    green-dominant, so a pixel's coverage is a = 1 - (g - max(r, b)) / 255,
    and over a backdrop B it becomes (r, min(g, max(r, b)), b) + (1 - a) B.
    """
    src = os.path.join(HERE, "footage", f"{clipset}-{name}.mp4")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-vf", f"fps={CLIP_FPS}",
                        os.path.join(tmp, "f%03d.png")], check=True)
        files = sorted(f for f in os.listdir(tmp) if f.endswith(".png"))
        backdrop = grid_backdrop(w * SS, h * SS).resize((w, h), Image.LANCZOS).convert("RGB")
        frames = []
        for f in files:
            im = Image.open(os.path.join(tmp, f)).convert("RGB")
            if clipset == "hands":
                im = strip_hud(im)
            # Fill the picture, cropping the take's longer side.
            k = max(w / im.width, h / im.height)
            im = im.resize((round(im.width * k), round(im.height * k)), Image.LANCZOS)
            x0, y0 = (im.width - w) // 2, (im.height - h) // 2
            im = im.crop((x0, y0, x0 + w, y0 + h))
            r, g, b = im.split()
            rb = ImageChops.lighter(r, b)
            green = ImageChops.subtract(g, rb)  # 255 (1 - a)
            fg = Image.merge("RGB", (r, ImageChops.darker(g, rb), b))
            behind = ImageChops.multiply(backdrop, Image.merge("RGB", (green, green, green)))
            frames.append(ImageChops.add(fg, behind))
    return frames


def clip_atlas(clipset, name, w, h):
    """Packs the clip's frames into a grid; it plays them forward, then back."""
    frames = keyed_frames(clipset, name, w, h)
    cols = math.ceil(math.sqrt(len(frames) * h / w))
    rows = math.ceil(len(frames) / cols)
    atlas = Image.new("RGB", (cols * w, rows * h))
    for i, fr in enumerate(frames):
        atlas.paste(fr, ((i % cols) * w, (i // cols) * h))
    path = os.path.join(OUT, f"place-clip-{clipset}-{name}.jpg")
    atlas.save(path, quality=88, optimize=True)
    print(f"{path}: {len(frames)} frames of {w}x{h}, {cols}x{rows}")
    return frames[0], len(frames), cols


# ---------------------------------------------------------------- boards


def board(hands, firsts=None):
    W, H = BOARD_W, BOARD_H
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Melee's menu frame: a red-orange rounded border round a dark panel,
    # and a slanted title tab with hatching on its right.
    top = BOARD_TOP
    d.rounded_rectangle((s(6), top, W - s(6), H - s(6)), radius=s(46), fill=(0, 0, 0, 205), outline=FRAME_RED, width=s(12))
    d.rounded_rectangle((s(22), top + s(16), W - s(22), H - s(22)), radius=s(34), outline=FRAME_DARK, width=s(3))
    tab = [(s(40), top + s(6)), (s(70), s(6)), (s(760), s(6)), (s(730), top + s(6))]
    d.polygon(tab, fill=(0, 0, 0, 230), outline=FRAME_RED, width=s(8))
    for i in range(7):
        x = s(790 + i * 30)
        d.polygon([(x, top), (x + s(30), s(14)), (x + s(44), s(14)), (x + s(14), top)], fill=FRAME_RED)
    italic_text(img, (s(104), s(54)), "Place the Stage", 44, SILVER, outline=(40, 40, 46))

    def still(name, fallback):
        # The hand cards show their clip's first frame until it plays.
        if firsts and name in firsts:
            return lambda w, h: firsts[name].resize((w, h), Image.LANCZOS).convert("RGBA")
        return fallback

    grab = ["Point at the stage, pinch,"] if hands else ["Point, hold ", ("btn", "GRIP")]
    both = ["Pinch with both hands"] if hands else ["Hold ", ("btn", "GRIP"), "on both"]
    cards = [
        ("MOVE", still("move", picture_move), [grab, ["and drag, near or far."]]),
        ("TURN", still("turn", picture_turn), [both, ["and twist."]]),
        ("SCALE", still("scale", picture_scale), [both, ["and pull apart or together."]]),
        ("READY?", picture_start, [
            [("btn", "A"), "or ", ("btn", "START"), "Fight!"],
            [("btn", "B"), "Reset the stage"],
            [("btn", "Y"), "Hide these tips"],
        ]),
    ]
    rects = []
    for i, (title, picture, lines) in enumerate(cards):
        c, (px, py, pw, ph) = card(CARD_W, CARD_H, title, picture, lines)
        img.alpha_composite(c, (card_x(i), CARD_Y))
        rects.append((card_x(i) + px, CARD_Y + py, pw, ph))
    return img, rects


def legend(hands):
    move = ("btn", "PINCH") if hands else ("btn", "GRIP")
    parts = [("btn", "A"), "Start   ", ("btn", "B"), "Reset   ", ("btn", "Y"), "Tips   ", move, " Move"]
    size = 30
    w = rich_width(parts, size) + s(80)
    h = s(84)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=h // 2, fill=(0, 0, 0, 215), outline=GOLD, width=s(5))
    rich_line(d, s(40), h // 2, parts, size)
    return img


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    out = img.resize((img.width // SS, img.height // SS), Image.LANCZOS)
    path = os.path.join(OUT, name)
    out.save(path, optimize=True)
    print(f"{path}: {out.width}x{out.height}")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    pw, ph = picture_size()
    w1, h1 = pw // SS, ph // SS  # clip frames: the picture's size on the saved card
    lines = []
    for clipset, hands, cards, legend_name in (("controllers", False, "place-cards.png", "place-legend.png"),
                                               ("hands", True, "place-cards-hands.png", "place-legend-hands.png")):
        firsts, index = {}, []
        for name in CLIPS:
            if not os.path.exists(os.path.join(HERE, "footage", f"{clipset}-{name}.mp4")):
                continue  # a still instead
            first, frames, cols = clip_atlas(clipset, name, w1, h1)
            firsts[name] = first
            index.append((name, frames, cols))
        img, rects = board(hands, firsts)
        save(img, cards)
        save(legend(hands), legend_name)
        for name, frames, cols in index:
            x, y, w, h = rects[CLIPS.index(name)]
            lines.append(f"{clipset} {name} {frames} {cols} {w1} {h1} {CLIP_FPS} {x // SS} {y // SS} {w // SS} {h // SS}")
    # set name frames cols frame_w frame_h fps x y w h (the picture on its
    # set's cards, 1x pixels); played forward, then back.
    with open(os.path.join(OUT, "place-clips.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
