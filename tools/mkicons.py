#!/usr/bin/env python3
"""Draws the SynthOS desktop icons and symbols and writes them as compressed C source.

Icons are 64x64 ARGB (squircle app icons, folders, documents); symbols are monochrome
alpha masks at 16 and 32 px that the desktop tints at run time.
usage: tools/mkicons.py > kernel/gui/icons.c (needs Pillow); the app icons themselves go to
rootfs/system/icons.pak, which the desktop reads at start, so they do not weigh on the kernel image
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lz4pack  # noqa: E402

S = 256
SIZE = 96
INTER_BOLD = "/usr/share/fonts/opentype/inter/Inter-Bold.otf"
MONO_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SANS_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.truetype(SANS_BOLD, size)


def blank(size=S):
    return Image.new("RGBA", (size, size), (0, 0, 0, 0))


def flat(size, top, bottom):
    """One even color between the two: icons have no light from above and no shade below."""
    return Image.new("RGBA", size, tuple((top[k] + bottom[k]) // 2 for k in range(4)))


def squircle_mask(box, n=5.0):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    k = 4
    big = Image.new("L", (w * k, h * k), 0)
    px = big.load()
    a, b = w * k / 2, h * k / 2
    for y in range(h * k):
        dy = abs((y + 0.5 - b) / b) ** n
        if dy >= 1:
            continue
        span = a * (1 - dy) ** (1 / n)
        for x in range(int(a - span), int(a + span) + 1):
            if 0 <= x < w * k:
                px[x, y] = 255
    small = big.resize((w, h), Image.LANCZOS)
    mask = Image.new("L", (S, S), 0)
    mask.paste(small, (x0, y0))
    return mask


def rounded_mask(box, radius):
    mask = Image.new("L", (S * 4, S * 4), 0)
    x0, y0, x1, y1 = box
    ImageDraw.Draw(mask).rounded_rectangle([x0 * 4, y0 * 4, x1 * 4, y1 * 4], radius=radius * 4, fill=255)
    return mask.resize((S, S), Image.LANCZOS)


BODY = (24, 24, 232, 232)


def gradient(size, top, bottom):
    """A vertical blend from top to bottom."""
    w, h = size
    ramp = Image.linear_gradient("L").resize((w, h))
    a = Image.new("RGBA", size, top)
    b = Image.new("RGBA", size, bottom)
    return Image.composite(b, a, ramp)


def lighter(c, k):
    return tuple(int(c[i] + (255 - c[i]) * k) for i in range(3)) + (c[3],)


def page_base(emblem=None):
    """A sheet with rounded corners: a white page over a soft shadow, the emblem in front."""
    mask = rounded_mask((52, 20, 204, 236), 26)
    img = blank()
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    shadow.putalpha(mask.point(lambda v: v * 50 // 255).filter(ImageFilter.GaussianBlur(7)))
    img.alpha_composite(shadow, (0, 5))
    img.paste(gradient((S, S), (255, 255, 255, 255), (236, 238, 244, 255)), (0, 0), mask)
    edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(3)))
    ring = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    ring.putalpha(edge.point(lambda v: v * 34 // 255))
    img.alpha_composite(ring)
    if emblem:
        emblem(img, ImageDraw.Draw(img))
    return img

def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


# ---------------------------------------------------------------- app icons

# ---------------------------------------------------------------- layered app icons
#
# Every app icon is a stack of layers on a rounded squircle, the way current macOS and iOS icons are built:
# a soft gradient ground, shapes far behind that are out of focus (blurred), frosted glass plates that blur
# whatever lies under them, and a crisp symbol in front lifted by a soft shadow. No shine along the rim.

class Scaled:
    """Draws on a 4x canvas with 256-unit coordinates, for smooth (antialiased) masks."""

    def __init__(self, d):
        self.d = d

    def __getattr__(self, name):
        orig = getattr(self.d, name)

        def call(*args, **kw):
            if name in ("arc", "pieslice", "chord"):
                args = [_scale4(args[0])] + list(args[1:])
            else:
                args = [_scale4(a) for a in args]
            for key in ("width", "radius"):
                if key in kw:
                    kw[key] = kw[key] * 4
            return orig(*args, **kw)
        return call


def _scale4(a):
    if isinstance(a, (list, tuple)):
        return type(a)(_scale4(v) for v in a)
    if isinstance(a, (int, float)):
        return a * 4
    return a


def mask_of(draw):
    """The coverage of what draw(d) paints with fill=255 / outline=255."""
    big = Image.new("L", (S * 4, S * 4), 0)
    draw(Scaled(ImageDraw.Draw(big)))
    return big.resize((S, S), Image.LANCZOS)


def colored(mask, color):
    img = Image.new("RGBA", (S, S), color[:3] + (255,))
    a = color[3] if len(color) > 3 else 255
    img.putalpha(mask.point(lambda v: v * a // 255))
    return img


def tint_mask(mask, top, bottom):
    img = gradient((S, S), top, bottom)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), mask))
    return img


class Icon:
    """Layers stack from back to front: blur() is out of focus far behind, glass() frosts what lies under it,
    fill() is crisp and opaque. Each plate gets a thin opaque rim of light, so the layers stay apart."""

    def __init__(self, top, bottom, box=BODY):
        self.mask = squircle_mask(box)
        self.img = blank()
        self.img.paste(gradient((S, S), top, bottom), (0, 0), self.mask)

    def blur(self, draw, color, radius, dx=0, dy=0):
        layer = colored(mask_of(draw), color).filter(ImageFilter.GaussianBlur(radius))
        self.img.alpha_composite(layer, (dx, dy)) if dx or dy else self.img.alpha_composite(layer)
        return self

    def rim(self, m, color, width=3):
        edge = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(width * 2 - 1)))
        fade = gradient((S, S), (255, 255, 255, 255), (255, 255, 255, 90)).getchannel("A")
        self.img.alpha_composite(colored(ImageChops.multiply(edge, fade), color))

    def fill(self, draw, top, bottom=None, shadow=0, rim=None):
        m = mask_of(draw)
        if shadow:
            sh = colored(m, (0, 0, 0, shadow)).filter(ImageFilter.GaussianBlur(6))
            self.img.alpha_composite(sh, (0, 5))
        self.img.alpha_composite(tint_mask(m, top, bottom or top))
        if rim:
            self.rim(m, rim, 2)
        return self

    def glass(self, draw, tint=(255, 255, 255, 70), blur=12, shadow=40, rim=(255, 255, 255, 200)):
        m = mask_of(draw)
        if shadow:
            sh = colored(m, (0, 0, 0, shadow)).filter(ImageFilter.GaussianBlur(9))
            self.img.alpha_composite(sh, (0, 7))
        under = self.img.filter(ImageFilter.GaussianBlur(blur))
        plate = Image.composite(under, blank(), m)
        plate.alpha_composite(colored(m, tint))
        light = gradient((S, S), (255, 255, 255, 46), (255, 255, 255, 0))
        light.putalpha(ImageChops.multiply(light.getchannel("A"), m))
        plate.alpha_composite(light)
        self.img.alpha_composite(plate)
        if rim:
            self.rim(m, rim)
        return self

    def done(self):
        out = Image.composite(self.img, blank(), self.mask)
        inner = ImageChops.subtract(self.mask.filter(ImageFilter.MinFilter(3)), self.mask.filter(ImageFilter.MinFilter(7)))
        fade = gradient((S, S), (255, 255, 255, 150), (255, 255, 255, 20)).getchannel("A")
        out.alpha_composite(colored(ImageChops.multiply(inner, fade), (255, 255, 255, 120)))
        edge = ImageChops.subtract(self.mask, self.mask.filter(ImageFilter.MinFilter(3)))
        out.alpha_composite(colored(edge, (0, 0, 0, 40)))
        return out



def rr4(box, r, outline=False, width=0):
    def draw(d):
        if outline:
            d.rounded_rectangle(box, radius=r, outline=255, width=width)
        else:
            d.rounded_rectangle(box, radius=r, fill=255)
    return draw


def ellipse4(box, outline=False, width=0):
    def draw(d):
        if outline:
            d.ellipse(box, outline=255, width=width)
        else:
            d.ellipse(box, fill=255)
    return draw


def poly4(pts):
    return lambda d: d.polygon(pts, fill=255)


def line4(pts, width):
    return lambda d: d.line(pts, fill=255, width=width, joint="curve")


def text4(s, path, size, xy):
    def draw(d):
        f = font(path, size * 4)
        d.d.text((xy[0] * 4, xy[1] * 4), s, font=f, fill=255)
    return draw


def gear_mask(cx, cy, r_out, r_in, teeth, hole):
    def draw(d):
        pts = []
        for i in range(teeth * 4):
            a = i * math.pi * 2 / (teeth * 4)
            r = r_out if (i % 4) in (0, 1) else r_in
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        d.polygon(pts, fill=255)
        d.ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=0)
    return draw


def gear_shape(d, cx, cy, r_out, r_in, teeth, fill, hole_r=0, hole=None):
    pts = []
    for i in range(teeth * 4):
        a = i * math.pi * 2 / (teeth * 4)
        r = r_out if (i % 4) in (0, 1) else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=fill)
    if hole_r:
        d.ellipse([cx - hole_r, cy - hole_r, cx + hole_r, cy + hole_r], fill=hole)


def calc_icon():
    icon = (Icon((70, 70, 78, 255), (22, 22, 26, 255))
            .blur(ellipse4((130, 20, 250, 140)), (255, 150, 20, 190), 24))
    for row in range(3):
        for col in range(3):
            x, y = 62 + col * 46, 62 + row * 46
            if col == 2:
                icon.fill(ellipse4((x, y, x + 38, y + 38)), (255, 176, 40, 255), (250, 140, 0, 255), 40)
            else:
                icon.glass(ellipse4((x, y, x + 38, y + 38)), (255, 255, 255, 46 if row else 80), 6, 0)
    return icon.done()


def mirage_mask(size, cx, cy, r):
    """The SynthOS mark: a sun whose lower half breaks into two dune waves that draw an S."""
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    for base, amp, thick, phase in ((0.10, 0.20, 0.15, 0.0), (0.50, 0.15, 0.12, math.pi)):
        top, bottom = [], []
        for i in range(0, 101):
            x = cx - r * 1.2 + 2.4 * r * i / 100
            t = (x - cx) / r
            y = cy + r * (base + amp * math.sin(t * math.pi * 0.9 + phase))
            top.append((x, y - r * thick / 2))
            bottom.append((x, y + r * thick / 2))
        d.polygon(top + bottom[::-1], fill=0)
    return m


WHITE = (255, 255, 255, 255)

# A quiet palette: grounds are paper, stone or graphite; each icon gets at most one muted accent.
PAPER = ((250, 250, 251, 255), (218, 219, 224, 255))
STONE = ((204, 206, 211, 255), (146, 148, 156, 255))
GRAPHITE = ((86, 88, 96, 255), (28, 29, 33, 255))
BLUE = (92, 136, 196)
RED = (206, 92, 86)
GREEN = (104, 172, 128)
AMBER = (224, 178, 92)
ROSE = (206, 98, 122)
TEAL = (90, 160, 168)
VIOLET = (132, 116, 186)
INK = (64, 66, 74, 255)
FROST = (255, 255, 255, 120)
SMOKE = (52, 54, 62, 150)


def a(c, alpha=255):
    return c + (alpha,)


def deeper(c, k=0.78):
    return tuple(int(v * k) for v in c[:3]) + (255,)


def top_rounded(box, r):
    return lambda d: d.rounded_rectangle(box, radius=r, fill=255, corners=(True, True, False, False))


def files_icon():
    return (Icon(*PAPER)
            .blur(rr4((44, 70, 212, 196), 30), a(BLUE, 150), 18, 0, 10)
            .fill(rr4((52, 64, 126, 104), 18), (176, 198, 228, 255), (150, 178, 216, 255))
            .glass(rr4((48, 84, 208, 200), 28), (196, 214, 238, 170), 12, 34)
            .fill(rr4((104, 128, 152, 141), 6.5), a(BLUE), deeper(BLUE))
            .done())


def terminal_icon():
    return (Icon(*GRAPHITE)
            .blur(ellipse4((126, 126, 236, 236)), (120, 196, 150, 80), 30)
            .glass(rr4((40, 52, 216, 204), 28), (50, 52, 60, 150), 10, 50, (255, 255, 255, 70))
            .fill(lambda d: d.line([(76, 100), (108, 126), (76, 152)], fill=255, width=14, joint="curve"),
                  (244, 244, 246, 255), (204, 206, 212, 255))
            .fill(ellipse4((69, 93, 83, 107)), (244, 244, 246, 255))
            .fill(ellipse4((69, 145, 83, 159)), (214, 216, 222, 255))
            .fill(rr4((120, 145, 176, 159), 7), (156, 216, 174, 255), (118, 190, 142, 255))
            .done())


def settings_icon():
    return (Icon(*STONE)
            .blur(gear_mask(150, 150, 110, 90, 14, 40), (70, 72, 80, 90), 14)
            .glass(ellipse4((50, 50, 206, 206)), (255, 255, 255, 70), 12, 30)
            .fill(gear_mask(128, 128, 64, 52, 12, 24), (248, 248, 250, 255), (206, 208, 214, 255), 60,
                  (255, 255, 255, 255))
            .fill(ellipse4((114, 114, 142, 142)), (120, 122, 130, 255), (86, 88, 96, 255))
            .done())


def notepad_icon():
    icon = (Icon(*PAPER)
            .blur(rr4((74, 42, 226, 184), 24), a(AMBER, 120), 16, 8, -4)
            .glass(rr4((50, 54, 206, 210), 28), (255, 255, 255, 190), 10, 34)
            .fill(top_rounded((50, 54, 206, 94), 28), a(AMBER), deeper(AMBER, 0.9)))
    for i in range(4):
        y = 122 + i * 22
        icon.fill(rr4((74, y - 3.5, 182 - (i % 2) * 34, y + 3.5), 3.5), (190, 192, 200, 255))
    return icon.done()


def paint_icon():
    icon = Icon(*PAPER)
    for (x, y, c) in [(40, 46, RED), (150, 36, AMBER), (156, 150, BLUE)]:
        icon.blur(ellipse4((x, y, x + 74, y + 74)), a(c, 150), 22)

    def palette(d):
        d.ellipse([44, 62, 212, 202], fill=255)
        d.ellipse([146, 140, 178, 172], fill=0)
    icon.glass(palette, (255, 255, 255, 150), 12, 34)
    for (x, y, c) in [(74, 98, RED), (110, 80, AMBER), (150, 86, GREEN), (78, 140, BLUE)]:
        icon.fill(ellipse4((x, y, x + 28, y + 28)), a(c), deeper(c, 0.85), 40)
    return icon.done()


def monitor_icon():
    pts = [(58, 168), (86, 148), (108, 160), (132, 102), (154, 132), (176, 86), (200, 108)]
    return (Icon(*GRAPHITE)
            .blur(line4(pts, 30), a(GREEN, 120), 16)
            .glass(rr4((40, 56, 216, 200), 28), (255, 255, 255, 22), 8, 0, (255, 255, 255, 60))
            .fill(line4(pts, 11), (176, 226, 192, 255), a(GREEN))
            .done())


def browser_icon():
    icon = (Icon(*PAPER)
            .blur(ellipse4((36, 36, 220, 220)), a(BLUE, 140), 20)
            .glass(ellipse4((46, 46, 210, 210)), (236, 241, 248, 130), 12, 34))
    for i in range(12):
        r = math.radians(i * 30)
        cx, cy = 128 + 66 * math.cos(r), 128 + 66 * math.sin(r)
        dx, dy = 8 * math.cos(r), 8 * math.sin(r)
        icon.fill(line4([(cx - dx, cy - dy), (cx, cy)], 4), (150, 154, 164, 255))
    return (icon.fill(poly4([(128, 128), (176, 80), (140, 140)]), a(RED), deeper(RED), 40)
            .fill(poly4([(128, 128), (80, 176), (116, 116)]), (236, 236, 240, 255), (196, 198, 206, 255), 40)
            .fill(ellipse4((120, 120, 136, 136)), INK)
            .done())


def mines_icon():
    cx, cy = 124, 138

    def mine(d):
        for ang in range(0, 360, 45):
            r = math.radians(ang)
            d.line([(cx, cy), (cx + 52 * math.cos(r), cy + 52 * math.sin(r))], fill=255, width=12)
        d.ellipse([cx - 38, cy - 38, cx + 38, cy + 38], fill=255)
    icon = Icon(*STONE)
    for i in range(3):
        for j in range(3):
            icon.glass(rr4((32 + i * 66, 32 + j * 66, 92 + i * 66, 92 + j * 66), 16), (255, 255, 255, 50), 8, 0,
                       (255, 255, 255, 110))
    return (icon.blur(mine, (0, 0, 0, 110), 10, 0, 8)
            .fill(mine, (66, 68, 76, 255), (28, 29, 33, 255))
            .fill(ellipse4((cx - 22, cy - 22, cx - 4, cy - 4)), (226, 228, 234, 255))
            .fill(rr4((180, 52, 188, 126), 4), (244, 244, 246, 255))
            .fill(poly4([(188, 54), (222, 72), (188, 90)]), a(RED), deeper(RED), 30)
            .done())


def launchpad_icon():
    tones = [(150, 154, 164), (196, 198, 206), a(BLUE)[:3], (196, 198, 206), (96, 98, 108), (170, 172, 180),
             a(AMBER)[:3], (170, 172, 180), (196, 198, 206)]
    icon = Icon(*PAPER)
    for i, c in enumerate(tones):
        x, y = 40 + (i % 3) * 62, 40 + (i // 3) * 62
        icon.blur(rr4((x, y, x + 52, y + 52), 16), c + (150,), 12)
    for i, c in enumerate(tones):
        x, y = 60 + (i % 3) * 50, 60 + (i // 3) * 50
        icon.fill(rr4((x, y, x + 38, y + 38), 13), c + (255,), deeper(c, 0.86), 30, (255, 255, 255, 120))
    return icon.done()


def logo_icon():
    icon = (Icon((118, 106, 132, 255), (42, 38, 52, 255))
            .blur(ellipse4((50, 40, 210, 200)), (255, 226, 176, 130), 28))
    m = mirage_mask((S, S), 128, 126, 66)
    sh = colored(m, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(7))
    icon.img.alpha_composite(sh, (0, 6))
    icon.img.alpha_composite(tint_mask(m, (255, 252, 244, 255), (236, 200, 146, 255)))
    return icon.done()


def photos_icon():
    return (Icon(*PAPER)
            .blur(rr4((40, 50, 216, 160), 40), a(BLUE, 120), 20)
            .blur(ellipse4((138, 48, 206, 116)), a(AMBER, 170), 14)
            .fill(ellipse4((150, 66, 186, 102)), (246, 222, 170, 255), a(AMBER))
            .glass(poly4([(36, 214), (102, 118), (148, 172), (176, 142), (222, 214)]), (255, 255, 255, 120), 10, 30)
            .glass(poly4([(70, 214), (132, 150), (196, 214)]), (120, 124, 134, 90), 8, 20, (255, 255, 255, 140))
            .done())


def music_icon():
    def note(d):
        d.polygon([(92, 70), (184, 48), (184, 74), (92, 96)], fill=255)
        d.line([(98, 80), (98, 170)], fill=255, width=14)
        d.line([(178, 60), (178, 150)], fill=255, width=14)
        d.ellipse([56, 150, 106, 192], fill=255)
        d.ellipse([136, 130, 186, 172], fill=255)
    return (Icon(*PAPER)
            .blur(note, a(ROSE, 120), 16, 12, 10)
            .fill(note, (222, 118, 140, 255), deeper(ROSE, 0.8), 50)
            .done())


def video_icon():
    return (Icon(*GRAPHITE)
            .blur(ellipse4((60, 60, 200, 200)), a(VIOLET, 120), 28)
            .glass(rr4((44, 66, 212, 190), 30), (255, 255, 255, 46), 12, 30, (255, 255, 255, 80))
            .fill(lambda d: d.polygon([(112, 100), (112, 156), (160, 128)], fill=255), (246, 246, 248, 255),
                  (210, 212, 218, 255), 50)
            .done())


def viewer_icon():
    return (Icon(*PAPER)
            .blur(ellipse4((40, 40, 190, 190)), a(TEAL, 120), 18)
            .fill(line4([(158, 158), (202, 202)], 24), (110, 112, 120, 255), (60, 62, 70, 255), 40)
            .glass(ellipse4((56, 56, 172, 172)), (240, 246, 248, 120), 12, 30)
            .fill(ellipse4((56, 56, 172, 172), True, 12), (120, 122, 132, 255), (78, 80, 90, 255))
            .done())


def installer_icon():
    return (Icon(*PAPER)
            .blur(rr4((40, 124, 216, 230), 30), (60, 62, 72, 120), 14)
            .glass(rr4((50, 148, 206, 208), 24), (255, 255, 255, 140), 10, 30)
            .fill(ellipse4((170, 170, 184, 184)), a(GREEN))
            .fill(rr4((114, 40, 142, 112), 14), a(BLUE), deeper(BLUE), 40)
            .fill(lambda d: d.polygon([(80, 96), (176, 96), (128, 146)], fill=255), a(BLUE), deeper(BLUE), 40)
            .done())


def exe_icon():
    icon = (Icon(*STONE)
            .blur(rr4((70, 40, 230, 170), 22), (255, 255, 255, 120), 14)
            .glass(rr4((46, 62, 210, 196), 22), (255, 255, 255, 160), 10, 34)
            .fill(top_rounded((46, 62, 210, 94), 22), (232, 233, 238, 255)))
    for i in range(3):
        icon.fill(ellipse4((62 + i * 22, 71, 76 + i * 22, 85)), (150, 152, 160, 255))
    return (icon.fill(rr4((64, 112, 132, 126), 7), a(BLUE))
            .fill(rr4((64, 140, 186, 149), 4.5), (180, 182, 190, 255))
            .fill(rr4((64, 160, 166, 169), 4.5), (180, 182, 190, 255))
            .done())


def screenshot_icon():
    def brackets(d):
        for x0, y0, dx, dy in ((52, 60, 1, 1), (204, 60, -1, 1), (52, 196, 1, -1), (204, 196, -1, -1)):
            d.line([(x0, y0 + dy * 34), (x0, y0), (x0 + dx * 34, y0)], fill=255, width=11, joint="curve")
    return (Icon(*STONE)
            .blur(ellipse4((60, 60, 200, 200)), (255, 255, 255, 120), 22)
            .fill(brackets, (250, 250, 252, 255), None, 30)
            .glass(rr4((82, 98, 174, 166), 18), (255, 255, 255, 120), 8)
            .fill(rr4((110, 88, 146, 106), 6), (250, 250, 252, 255))
            .fill(ellipse4((110, 114, 146, 150)), (80, 82, 92, 255), (50, 52, 60, 255))
            .fill(ellipse4((120, 124, 136, 140)), (190, 204, 224, 255))
            .done())


def computer_icon():
    return (Icon((238, 238, 242, 255), (180, 180, 188, 255), (20, 60, 236, 196))
            .blur(rr4((40, 80, 216, 180), 24), (255, 255, 255, 180), 12)
            .glass(rr4((34, 74, 222, 150), 20), (255, 255, 255, 70), 8, 0)
            .fill(rr4((44, 150, 212, 153), 1.5), (156, 158, 166, 255))
            .fill(ellipse4((186, 160, 200, 174)), a(GREEN))
            .done())


def usb_icon():
    cx, cy = 128, 132

    def mark(d):
        d.line([(cx, cy - 40), (cx, cy + 40)], fill=255, width=9)
        d.polygon([(cx - 15, cy - 34), (cx + 15, cy - 34), (cx, cy - 58)], fill=255)
        d.line([(cx, cy + 6), (cx - 30, cy - 14), (cx - 30, cy - 26)], fill=255, width=8, joint="curve")
        d.line([(cx, cy + 20), (cx + 30, cy), (cx + 30, cy - 12)], fill=255, width=8, joint="curve")
        d.ellipse([cx - 13, cy + 33, cx + 13, cy + 59], fill=255)
        d.ellipse([cx - 38, cy - 36, cx - 22, cy - 20], fill=255)
        d.rectangle([cx + 22, cy - 26, cx + 38, cy - 10], fill=255)
    return (Icon(*STONE)
            .blur(ellipse4((70, 70, 190, 190)), a(AMBER, 120), 22)
            .glass(rr4((56, 56, 200, 200), 34), (255, 255, 255, 60), 10, 20)
            .fill(mark, (252, 252, 253, 255), (222, 224, 230, 255), 50)
            .done())



FOLDER_TOP = (176, 202, 232, 255)
FOLDER_BOTTOM = (128, 162, 206, 255)


def folder_base(emblem=None):
    """A folder of two layers: the back with its tab, and a frosted front plate that blurs it."""
    img = blank()
    back = rounded_mask((24, 52, 232, 214), 22)
    tab = rounded_mask((24, 38, 108, 80), 16)
    back_layer = gradient((S, S), (132, 164, 204, 255), (100, 132, 178, 255))
    img.paste(back_layer, (0, 0), ImageChops.lighter(back, tab))
    sheet = colored(rounded_mask((40, 62, 216, 150), 10), (255, 255, 255, 220))
    img.alpha_composite(sheet.filter(ImageFilter.GaussianBlur(2)))
    front = rounded_mask((24, 80, 232, 220), 22)
    under = img.filter(ImageFilter.GaussianBlur(10))
    plate = Image.composite(under, blank(), front)
    plate.alpha_composite(tint_mask(front, FOLDER_TOP[:3] + (200,), FOLDER_BOTTOM[:3] + (230,)))
    edge = ImageChops.subtract(front, front.filter(ImageFilter.MinFilter(3)))
    plate.alpha_composite(colored(edge, (255, 255, 255, 170)))
    shadow = colored(front, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(6))
    img.alpha_composite(shadow, (0, -2))
    img.alpha_composite(plate)
    if emblem:
        e = blank()
        emblem(e, ImageDraw.Draw(e))
        tint = Image.new("RGBA", (S, S), (76, 108, 152, 255))
        tint.putalpha(e.getchannel("A").point(lambda v: v * 200 // 255))
        img.alpha_composite(tint)
    return img


def circle_badge(c_top, c_bottom, draw):
    m = mask_of(ellipse4((30, 30, 226, 226)))
    img = blank()
    img.paste(gradient((S, S), c_top, c_bottom), (0, 0), m)
    glow = colored(mask_of(ellipse4((50, 30, 200, 150))), (255, 255, 255, 90)).filter(ImageFilter.GaussianBlur(20))
    img.alpha_composite(Image.composite(glow, blank(), m))
    g = blank()
    draw(g, ImageDraw.Draw(g))
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    sh.putalpha(g.getchannel("A").point(lambda v: v * 60 // 255).filter(ImageFilter.GaussianBlur(6)))
    img.alpha_composite(Image.composite(sh, blank(), m), (0, 0))
    img.alpha_composite(g)
    return Image.composite(img, blank(), m)


def folder_icon():
    return folder_base()


def home_folder_icon():
    def emblem(img, d):
        d.polygon([(128, 100), (178, 146), (166, 146), (166, 190), (90, 190), (90, 146), (78, 146)], fill=(0, 0, 0, 255))
    return folder_base(emblem)


def apps_folder_icon():
    def emblem(img, d):
        d.line([(100, 192), (128, 112), (156, 192)], fill=(0, 0, 0, 255), width=12)
        d.line([(108, 166), (148, 166)], fill=(0, 0, 0, 255), width=10)
    return folder_base(emblem)


def docs_folder_icon():
    def emblem(img, d):
        rr(d, (100, 108, 156, 192), 6, outline=(0, 0, 0, 255), width=8)
        for y in (130, 148, 166):
            d.line([(110, y), (146, y)], fill=(0, 0, 0, 255), width=6)
    return folder_base(emblem)


def desktop_folder_icon():
    def emblem(img, d):
        rr(d, (90, 112, 166, 168), 6, outline=(0, 0, 0, 255), width=8)
        d.line([(128, 168), (128, 186)], fill=(0, 0, 0, 255), width=8)
        d.line([(108, 190), (148, 190)], fill=(0, 0, 0, 255), width=8)
    return folder_base(emblem)


def downloads_folder_icon():
    def emblem(img, d):
        d.ellipse([92, 112, 164, 184], outline=(0, 0, 0, 255), width=8)
        d.line([(128, 124), (128, 170)], fill=(0, 0, 0, 255), width=8)
        d.line([(110, 152), (128, 172), (146, 152)], fill=(0, 0, 0, 255), width=8)
    return folder_base(emblem)


def text_doc():
    def emblem(img, d):
        for i, w in enumerate([90, 104, 80, 98, 60, 92, 70]):
            y = 92 + i * 18
            d.line([(76, y), (76 + w, y)], fill=(160, 166, 178, 255), width=6)
    return page_base(emblem)


def blank_doc():
    return page_base()


def image_doc():
    def emblem(img, d):
        rr(d, (72, 96, 184, 196), 14, fill=(170, 196, 228, 255))
        d.polygon([(72, 196), (112, 140), (146, 176), (162, 158), (184, 196)], fill=(122, 156, 136, 255))
        d.ellipse([146, 108, 170, 132], fill=(232, 196, 120, 255))
    return page_base(emblem)


def dll_doc():
    def emblem(img, d):
        gear_shape(d, 128, 148, 48, 38, 10, (130, 136, 150, 255), 16, (255, 255, 255, 255))
    return page_base(emblem)


def dos_doc():
    def emblem(img, d):
        rr(d, (70, 100, 186, 190), 16, fill=(44, 45, 50, 255))
        f = font(MONO_BOLD, 34)
        d.text((80, 126), "C:\\>", font=f, fill=(200, 200, 205, 255))
    return page_base(emblem)


def sx_doc():
    def emblem(img, d):
        rr(d, (78, 110, 178, 180), 18, fill=(126, 112, 178, 255))
        f = font(INTER_BOLD, 44)
        d.text((92, 118), "SX", font=f, fill=(255, 255, 255, 255))
    return page_base(emblem)


def script_doc():
    def emblem(img, d):
        rr(d, (74, 106, 182, 184), 16, fill=(44, 45, 50, 255))
        f = font(MONO_BOLD, 40)
        d.text((88, 118), ">_", font=f, fill=(170, 222, 184, 255))
    return page_base(emblem)


def info_icon():
    def draw(img, d):
        d.ellipse([116, 62, 140, 86], fill=(255, 255, 255, 255))
        rr(d, (114, 102, 142, 194), 10, fill=(255, 255, 255, 255))
    return circle_badge((130, 168, 218, 255), (78, 120, 184, 255), draw)


def power_icon():
    def draw(img, d):
        d.arc([76, 76, 180, 180], 300, 240, fill=(255, 255, 255, 255), width=14)
        d.line([(128, 60), (128, 128)], fill=(255, 255, 255, 255), width=14)
    return circle_badge((222, 120, 112, 255), (182, 74, 70, 255), draw)


def warning_icon():
    img = blank()
    mask = Image.new("L", (S * 4, S * 4), 0)
    md = ImageDraw.Draw(mask)
    tri = [(128, 34), (230, 210), (26, 210)]
    md.polygon([(x * 4, y * 4) for x, y in tri], fill=255)
    md.line([(x * 4, y * 4) for x, y in tri + tri[:1]], fill=255, width=40, joint="curve")
    mask = mask.resize((S, S), Image.LANCZOS)
    img.paste(flat((S, S), (240, 204, 100, 255), (226, 176, 70, 255)), (0, 0), mask)
    d = ImageDraw.Draw(img)
    rr(d, (118, 92, 138, 166), 10, fill=(60, 54, 44, 255))
    d.ellipse([116, 176, 140, 200], fill=(60, 54, 44, 255))
    return img


def user_icon():
    def draw(img, d):
        d.ellipse([88, 62, 168, 142], fill=(255, 255, 255, 255))
        d.ellipse([52, 150, 204, 290], fill=(255, 255, 255, 255))
    img = blank()
    mask = Image.new("L", (S * 4, S * 4), 0)
    ImageDraw.Draw(mask).ellipse([16 * 4, 16 * 4, 240 * 4, 240 * 4], fill=255)
    mask = mask.resize((S, S), Image.LANCZOS)
    body = flat((S, S), (176, 180, 190, 255), (128, 132, 142, 255))
    img.paste(body, (0, 0), mask)
    g = blank()
    draw(g, ImageDraw.Draw(g))
    clip = blank()
    clip.paste(g, (0, 0), mask)
    img.alpha_composite(clip)
    return img


def trash_icon(full=False):
    """A tapered bin with a rounded foot: a thick oval rim over the dark opening, rounded slots pressed into
    the side, and a soft cylinder light; when full, crumpled paper shows over the rim."""
    img = blank()
    img.alpha_composite(colored(mask_of(ellipse4((70, 210, 186, 236))), (0, 0, 0, 70))
                        .filter(ImageFilter.GaussianBlur(6)))
    tl, tr, bl, br, ty, by = 60, 196, 80, 176, 78, 222

    def body_draw(d):
        d.polygon([(tl, ty), (tr, ty), (br + 1, by - 22), (bl - 1, by - 22)], fill=255)
        d.rounded_rectangle((bl - 1, by - 44, br + 1, by), radius=22, fill=255)
    body = mask_of(body_draw)
    profile = Image.new("L", (S, 1))
    for x in range(S):
        t = (x - 128) / 74.0
        v = 1 - min(1.0, abs(t)) ** 2.2
        hl = math.exp(-((t + 0.34) / 0.16) ** 2)
        profile.putpixel((x, 0), int(min(255, 178 + 66 * v + 22 * hl)))
    shade = profile.resize((S, S))
    grey = Image.merge("RGBA", (shade.point(lambda v: v - 4), shade.point(lambda v: v - 2), shade,
                                Image.new("L", (S, S), 255)))
    layer = Image.composite(grey, blank(), body)
    fall = gradient((S, S), (0, 0, 0, 0), (0, 0, 0, 40))
    layer.alpha_composite(Image.composite(fall, blank(), body))
    slots = Image.new("L", (S, S), 0)
    n = 7
    for i in range(n):
        f = (i + 0.5) / n
        x0, x1 = tl + 16 + (tr - tl - 32) * f, bl + 12 + (br - bl - 24) * f
        slots = ImageChops.lighter(slots, mask_of(line4([(x0, 104), (x1, 196)], 8)))
        slots = ImageChops.lighter(slots, mask_of(ellipse4((x0 - 4.5, 99.5, x0 + 4.5, 108.5))))
        slots = ImageChops.lighter(slots, mask_of(ellipse4((x1 - 4.5, 191.5, x1 + 4.5, 200.5))))
    lip = ImageChops.subtract(ImageChops.offset(slots, 2, 2), slots)
    layer.alpha_composite(colored(ImageChops.multiply(lip, body), (255, 255, 255, 200)))
    layer.alpha_composite(tint_mask(ImageChops.multiply(slots, body), (158, 162, 172, 255), (182, 186, 194, 255)))
    inner = ImageChops.subtract(slots, ImageChops.offset(slots, 0, 3))
    layer.alpha_composite(colored(ImageChops.multiply(inner, body), (80, 84, 94, 110)))
    edge = ImageChops.subtract(body, body.filter(ImageFilter.MinFilter(3)))
    layer.alpha_composite(colored(edge, (70, 74, 84, 110)))
    img.alpha_composite(layer)
    rim_box, hole_box = (50, 58, 206, 100), (64, 65, 192, 91)
    rim, hole = mask_of(ellipse4(rim_box)), mask_of(ellipse4(hole_box))
    img.alpha_composite(tint_mask(rim, (252, 252, 253, 255), (200, 202, 210, 255)))
    img.alpha_composite(tint_mask(hole, (52, 54, 62, 255), (104, 108, 118, 255)))
    if full:
        paper = Image.new("RGBA", (S * 4, S * 4), (0, 0, 0, 0))
        pd = Scaled(ImageDraw.Draw(paper))
        balls = [(98, 66, 24, (252, 252, 252, 255), 1), (134, 56, 28, (240, 238, 232, 255), 2),
                 (168, 68, 22, (230, 233, 240, 255), 3)]
        for cx, cy, r, c, seed in balls:
            pts = []
            for i in range(14):
                ang = math.tau * i / 14
                jit = 0.8 + 0.2 * ((i * 7 + seed * 5) % 5) / 4
                pts.append((cx + math.cos(ang) * r * jit, cy + math.sin(ang) * r * jit * 0.9))
            pd.polygon(pts, fill=c)
            pd.line(pts + [pts[0]], fill=(160, 164, 174, 255), width=2)
            for i in range(3):
                ang = math.tau * (i * 0.31 + seed * 0.17)
                x0, y0 = cx + math.cos(ang) * r * 0.55, cy + math.sin(ang) * r * 0.5
                pd.line([(x0, y0), (cx + math.cos(ang + 2.2) * r * 0.2, cy + math.sin(ang + 2.2) * r * 0.2)],
                        fill=(184, 188, 198, 255), width=2)
        paper = paper.resize((S, S), Image.LANCZOS)
        clip = ImageChops.lighter(mask_of(rr4((0, 0, 256, 78), 0)), hole)
        img.alpha_composite(Image.composite(paper, blank(), clip))
    front = ImageChops.multiply(ImageChops.subtract(rim, hole), mask_of(rr4((0, 77, 256, 256), 0)))
    img.alpha_composite(tint_mask(front, (250, 250, 252, 255), (212, 214, 222, 255)))
    hole_edge = ImageChops.subtract(hole.filter(ImageFilter.MaxFilter(3)), hole)
    img.alpha_composite(colored(ImageChops.multiply(hole_edge, mask_of(rr4((0, 0, 256, 79), 0))), (150, 154, 164, 200)))
    rim_edge = ImageChops.subtract(rim, rim.filter(ImageFilter.MinFilter(3)))
    img.alpha_composite(colored(rim_edge, (90, 94, 104, 130)))
    return img


def trash_full_icon():
    return trash_icon(True)


ICONS = [
    ("ICON_FOLDER", folder_icon), ("ICON_FILE", blank_doc), ("ICON_TEXT", text_doc), ("ICON_APP", exe_icon),
    ("ICON_DLL", dll_doc), ("ICON_IMAGE", image_doc), ("ICON_TERMINAL", terminal_icon),
    ("ICON_SETTINGS", settings_icon), ("ICON_CALC", calc_icon), ("ICON_NOTEPAD", notepad_icon),
    ("ICON_PAINT", paint_icon), ("ICON_COMPUTER", computer_icon), ("ICON_HOME", home_folder_icon),
    ("ICON_MONITOR", monitor_icon), ("ICON_MINES", mines_icon), ("ICON_DOS", dos_doc), ("ICON_SX", sx_doc),
    ("ICON_SCRIPT", script_doc), ("ICON_POWER", power_icon), ("ICON_LOGO", logo_icon), ("ICON_INFO", info_icon),
    ("ICON_FILES", files_icon), ("ICON_LAUNCHPAD", launchpad_icon), ("ICON_WARNING", warning_icon),
    ("ICON_USER", user_icon), ("ICON_TRASH", trash_icon), ("ICON_TRASH_FULL", trash_full_icon),
    ("ICON_USB", usb_icon), ("ICON_FOLDER_APPS", apps_folder_icon), ("ICON_FOLDER_DOCS", docs_folder_icon),
    ("ICON_FOLDER_DESKTOP", desktop_folder_icon), ("ICON_FOLDER_DOWNLOADS", downloads_folder_icon),
    ("ICON_BROWSER", browser_icon), ("ICON_PHOTOS", photos_icon), ("ICON_MUSIC", music_icon),
    ("ICON_VIDEO", video_icon), ("ICON_VIEWER", viewer_icon), ("ICON_INSTALLER", installer_icon),
    ("ICON_SCREENSHOT", screenshot_icon),
]

# ---------------------------------------------------------------- symbols (drawn on a 128 grid)

W = 9


def sym_logo(d):
    d.ellipse([12, 12, 116, 116], fill=255)
    for base, amp, thick, phase in ((0.10, 0.20, 0.17, 0.0), (0.50, 0.15, 0.14, math.pi)):
        top, bottom = [], []
        for i in range(0, 41):
            x = -2 + 132 * i / 40
            t = (x - 64) / 52
            y = 64 + 52 * (base + amp * math.sin(t * math.pi * 0.9 + phase))
            top.append((x, y - 52 * thick / 2))
            bottom.append((x, y + 52 * thick / 2))
        d.polygon(top + bottom[::-1], fill=0)


def sym_search(d):
    d.ellipse([18, 18, 84, 84], outline=255, width=W + 2)
    d.line([(76, 76), (110, 110)], fill=255, width=W + 4)


def sym_control(d):
    for y, on in ((36, True), (90, False)):
        d.rounded_rectangle([14, y - 18, 114, y + 18], radius=18, outline=255, width=W)
        cx = 92 if on else 36
        d.ellipse([cx - 12, y - 12, cx + 12, y + 12], fill=255)


def sym_wifi(d):
    for r in (78, 52, 26):
        d.arc([64 - r, 100 - r, 64 + r, 100 + r], 225, 315, fill=255, width=W + 2)
    d.ellipse([56, 92, 72, 108], fill=255)


def sym_home(d):
    d.line([(14, 62), (64, 18), (114, 62)], fill=255, width=W + 2, joint="curve")
    d.line([(30, 52), (30, 110), (98, 110), (98, 52)], fill=255, width=W, joint="curve")
    d.rectangle([54, 74, 74, 110], fill=255)


def sym_desktop(d):
    d.rounded_rectangle([12, 18, 116, 88], radius=8, outline=255, width=W)
    d.line([(64, 88), (64, 108)], fill=255, width=W)
    d.line([(36, 110), (92, 110)], fill=255, width=W)


def sym_doc(d):
    d.line([(30, 12), (78, 12), (104, 38), (104, 116), (30, 116), (30, 12)], fill=255, width=W, joint="curve")
    d.line([(76, 12), (76, 40), (104, 40)], fill=255, width=W - 2)
    for y in (60, 78, 96):
        d.line([(46, y), (88, y)], fill=255, width=W - 3)


def sym_apps(d):
    for i in range(9):
        x, y = 16 + (i % 3) * 36, 16 + (i // 3) * 36
        d.rounded_rectangle([x, y, x + 26, y + 26], radius=7, fill=255)


def sym_download(d):
    d.ellipse([12, 12, 116, 116], outline=255, width=W)
    d.line([(64, 34), (64, 90)], fill=255, width=W)
    d.line([(40, 68), (64, 92), (88, 68)], fill=255, width=W, joint="curve")


def sym_disk(d):
    d.rounded_rectangle([10, 36, 118, 96], radius=14, outline=255, width=W)
    d.ellipse([88, 60, 100, 72], fill=255)


def sym_trash(d):
    d.line([(14, 30), (114, 30)], fill=255, width=W)
    d.line([(46, 30), (50, 14), (78, 14), (82, 30)], fill=255, width=W - 2)
    d.line([(26, 34), (34, 116), (94, 116), (102, 34)], fill=255, width=W, joint="curve")
    for x in (52, 76):
        d.line([(x, 50), (x, 98)], fill=255, width=W - 3)


def sym_folder(d):
    d.line([(12, 104), (12, 22), (50, 22), (60, 36), (116, 36), (116, 104), (12, 104)], fill=255, width=W, joint="curve")
    d.line([(12, 52), (116, 52)], fill=255, width=W - 2)


def sym_clock(d):
    d.ellipse([12, 12, 116, 116], outline=255, width=W)
    d.line([(64, 30), (64, 66), (88, 80)], fill=255, width=W, joint="curve")


def sym_gear(d):
    pts = []
    for i in range(32):
        a = i * math.pi * 2 / 32
        r = 58 if (i % 4) in (0, 1) else 44
        pts.append((64 + r * math.cos(a), 64 + r * math.sin(a)))
    d.polygon(pts, fill=255)
    d.ellipse([42, 42, 86, 86], fill=0)


def sym_user(d):
    d.ellipse([40, 12, 88, 60], fill=255)
    d.pieslice([16, 70, 112, 166], 180, 360, fill=255)


def sym_users(d):
    d.ellipse([22, 18, 62, 58], fill=255)
    d.pieslice([4, 66, 80, 142], 180, 360, fill=255)
    d.ellipse([74, 28, 106, 60], fill=255)
    d.pieslice([62, 70, 122, 130], 180, 360, fill=255)


def sym_lock(d):
    d.arc([36, 10, 92, 66], 180, 360, fill=255, width=W + 2)
    d.line([(36, 38), (36, 56)], fill=255, width=W + 2)
    d.line([(92, 38), (92, 56)], fill=255, width=W + 2)
    d.rounded_rectangle([20, 54, 108, 118], radius=12, fill=255)


def sym_power(d):
    d.arc([18, 22, 110, 114], 300, 240, fill=255, width=W + 2)
    d.line([(64, 10), (64, 62)], fill=255, width=W + 2)


def sym_restart(d):
    d.arc([16, 16, 112, 112], 20, 320, fill=255, width=W + 2)
    d.polygon([(96, 6), (118, 34), (84, 42)], fill=255)


def sym_moon(d):
    d.ellipse([14, 14, 114, 114], fill=255)
    d.ellipse([44, 2, 136, 94], fill=0)


def sym_sun(d):
    d.ellipse([36, 36, 92, 92], fill=255)
    for a in range(0, 360, 45):
        r = math.radians(a)
        d.line([(64 + 44 * math.cos(r), 64 + 44 * math.sin(r)), (64 + 60 * math.cos(r), 64 + 60 * math.sin(r))],
               fill=255, width=W)


def sym_display(d):
    d.rounded_rectangle([8, 16, 120, 92], radius=10, outline=255, width=W)
    d.line([(64, 92), (64, 110)], fill=255, width=W)
    d.line([(34, 112), (94, 112)], fill=255, width=W)
    d.ellipse([46, 36, 82, 72], fill=255)


def sym_dock(d):
    d.rounded_rectangle([8, 16, 120, 112], radius=12, outline=255, width=W)
    d.rounded_rectangle([24, 84, 104, 102], radius=6, fill=255)


def sym_photo(d):
    d.rounded_rectangle([8, 18, 120, 110], radius=12, outline=255, width=W)
    d.polygon([(20, 100), (54, 56), (78, 84), (92, 70), (110, 100)], fill=255)
    d.ellipse([80, 32, 100, 52], fill=255)


def sym_appearance(d):
    d.ellipse([12, 12, 116, 116], outline=255, width=W)
    d.pieslice([12, 12, 116, 116], 90, 270, fill=255)


def sym_keyboard(d):
    d.rounded_rectangle([6, 30, 122, 100], radius=10, outline=255, width=W)
    for row in range(2):
        for col in range(5):
            x, y = 22 + col * 18, 46 + row * 18
            d.rectangle([x, y, x + 10, y + 10], fill=255)
    d.rectangle([36, 82, 92, 90], fill=255)


def sym_mouse(d):
    d.rounded_rectangle([34, 8, 94, 120], radius=30, outline=255, width=W)
    d.line([(64, 20), (64, 48)], fill=255, width=W)


def sym_info(d):
    d.ellipse([10, 10, 118, 118], outline=255, width=W)
    d.ellipse([56, 30, 72, 46], fill=255)
    d.rounded_rectangle([56, 54, 72, 98], radius=6, fill=255)


def chevron(d, pts):
    d.line(pts, fill=255, width=W + 3, joint="curve")


def sym_left(d):
    chevron(d, [(82, 20), (40, 64), (82, 108)])


def sym_right(d):
    chevron(d, [(46, 20), (88, 64), (46, 108)])


def sym_down(d):
    chevron(d, [(20, 44), (64, 86), (108, 44)])


def sym_up(d):
    chevron(d, [(20, 86), (64, 44), (108, 86)])


def sym_grid(d):
    for i in range(4):
        x, y = 16 + (i % 2) * 52, 16 + (i // 2) * 52
        d.rounded_rectangle([x, y, x + 42, y + 42], radius=8, outline=255, width=W)


def sym_list(d):
    for y in (26, 64, 102):
        d.ellipse([10, y - 7, 24, y + 7], fill=255)
        d.line([(38, y), (118, y)], fill=255, width=W)


def sym_plus(d):
    d.line([(64, 14), (64, 114)], fill=255, width=W + 3)
    d.line([(14, 64), (114, 64)], fill=255, width=W + 3)


def sym_minus(d):
    d.line([(14, 64), (114, 64)], fill=255, width=W + 3)


def sym_check(d):
    d.line([(18, 66), (50, 98), (112, 30)], fill=255, width=W + 5, joint="curve")


def sym_close(d):
    d.line([(24, 24), (104, 104)], fill=255, width=W + 4)
    d.line([(104, 24), (24, 104)], fill=255, width=W + 4)


def sym_pencil(d):
    d.line([(26, 102), (100, 28)], fill=255, width=22)
    d.polygon([(10, 118), (16, 92), (36, 112)], fill=255)


def sym_brush(d):
    d.line([(60, 68), (114, 14)], fill=255, width=14)
    d.ellipse([12, 66, 62, 116], fill=255)


def sym_eraser(d):
    d.polygon([(14, 84), (70, 28), (114, 72), (58, 128 - 12)], outline=255, width=W)
    d.polygon([(14, 84), (42, 56), (86, 100), (58, 116)], fill=255)


def sym_line(d):
    d.line([(16, 112), (112, 16)], fill=255, width=W + 2)


def sym_rect(d):
    d.rounded_rectangle([12, 26, 116, 102], radius=6, outline=255, width=W + 1)


def sym_ellipse(d):
    d.ellipse([10, 24, 118, 104], outline=255, width=W + 1)


def sym_bucket(d):
    d.polygon([(20, 56), (64, 12), (108, 56), (64, 100)], outline=255, width=W)
    d.polygon([(20, 56), (108, 56), (64, 100)], fill=255)
    d.ellipse([96, 82, 120, 114], fill=255)


def sym_dropper(d):
    d.line([(20, 108), (78, 50)], fill=255, width=14)
    d.ellipse([72, 12, 116, 56], fill=255)


def sym_undo(d):
    d.arc([30, 30, 116, 116], 180, 90, fill=255, width=W + 2)
    d.polygon([(8, 72), (30, 40), (52, 72)], fill=255)


def sym_terminal(d):
    d.rounded_rectangle([6, 14, 122, 114], radius=12, outline=255, width=W)
    d.line([(26, 44), (50, 64), (26, 84)], fill=255, width=W, joint="curve")
    d.line([(60, 88), (96, 88)], fill=255, width=W)


def sym_chip(d):
    d.rounded_rectangle([26, 26, 102, 102], radius=10, outline=255, width=W)
    d.rectangle([48, 48, 80, 80], fill=255)
    for i in range(3):
        p = 42 + i * 22
        for a, b in (((p, 6), (p, 26)), ((p, 102), (p, 122)), ((6, p), (26, p)), ((102, p), (122, p))):
            d.line([a, b], fill=255, width=W - 2)


def sym_memory(d):
    d.rounded_rectangle([6, 34, 122, 88], radius=6, outline=255, width=W)
    for i in range(4):
        x = 20 + i * 26
        d.rectangle([x, 48, x + 14, 74], fill=255)
    for i in range(8):
        x = 14 + i * 14
        d.line([(x, 88), (x, 104)], fill=255, width=5)


def sym_network(d):
    d.ellipse([10, 10, 118, 118], outline=255, width=W)
    d.ellipse([40, 10, 88, 118], outline=255, width=W - 2)
    d.line([(10, 64), (118, 64)], fill=255, width=W - 2)
    d.line([(20, 36), (108, 36)], fill=255, width=W - 3)
    d.line([(20, 92), (108, 92)], fill=255, width=W - 3)


def sym_sidebar(d):
    d.rounded_rectangle([6, 16, 122, 112], radius=12, outline=255, width=W)
    d.line([(46, 16), (46, 112)], fill=255, width=W)


def sym_star(d):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 58 if i % 2 == 0 else 24
        pts.append((64 + r * math.cos(a), 68 + r * math.sin(a)))
    d.polygon(pts, fill=255)


def sym_newfolder(d):
    sym_folder(d)
    d.line([(64, 62), (64, 96)], fill=255, width=W - 1)
    d.line([(47, 79), (81, 79)], fill=255, width=W - 1)


def sym_usb(d):
    d.line([(64, 14), (64, 100)], fill=255, width=W)
    d.polygon([(50, 28), (78, 28), (64, 6)], fill=255)
    d.line([(64, 64), (30, 44), (30, 30)], fill=255, width=W - 2)
    d.line([(64, 80), (98, 60), (98, 46)], fill=255, width=W - 2)
    d.ellipse([50, 96, 78, 124], fill=255)


def sym_battery(d):
    d.rounded_rectangle([6, 36, 110, 92], radius=12, outline=255, width=W)
    d.rounded_rectangle([20, 50, 82, 78], radius=6, fill=255)
    d.rectangle([112, 54, 122, 74], fill=255)


def sym_eject(d):
    d.polygon([(64, 14), (114, 72), (14, 72)], fill=255)
    d.rounded_rectangle([14, 90, 114, 110], radius=4, fill=255)


def sym_music(d):
    d.polygon([(44, 26), (104, 12), (104, 30), (44, 44)], fill=255)
    d.line([(48, 30), (48, 96)], fill=255, width=W)
    d.line([(100, 18), (100, 82)], fill=255, width=W)
    d.ellipse([18, 82, 52, 110], fill=255)
    d.ellipse([70, 68, 104, 96], fill=255)


def sym_video(d):
    d.rounded_rectangle([8, 30, 88, 98], radius=14, outline=255, width=W)
    d.polygon([(92, 56), (120, 36), (120, 92), (92, 72)], fill=255)


def sym_play(d):
    d.polygon([(34, 16), (110, 64), (34, 112)], fill=255)


def sym_pause(d):
    d.rounded_rectangle([28, 18, 54, 110], radius=6, fill=255)
    d.rounded_rectangle([74, 18, 100, 110], radius=6, fill=255)


def sym_prev(d):
    d.rounded_rectangle([16, 22, 32, 106], radius=4, fill=255)
    d.polygon([(110, 22), (110, 106), (38, 64)], fill=255)


def sym_next(d):
    d.rounded_rectangle([96, 22, 112, 106], radius=4, fill=255)
    d.polygon([(18, 22), (18, 106), (90, 64)], fill=255)


def sym_volume(d):
    d.polygon([(10, 46), (34, 46), (64, 18), (64, 110), (34, 82), (10, 82)], fill=255)
    for r in (22, 42):
        d.arc([64 - r, 64 - r, 64 + r, 64 + r], 300, 60, fill=255, width=W)


def sym_more(d):
    for x in (24, 64, 104):
        d.ellipse([x - 12, 52, x + 12, 76], fill=255)


def sym_weather(d):
    d.ellipse([20, 14, 76, 70], outline=255, width=W)
    d.ellipse([34, 54, 82, 102], fill=255)
    d.ellipse([62, 40, 116, 94], fill=255)
    d.rounded_rectangle([20, 74, 120, 112], radius=19, fill=255)


def sym_calendar(d):
    d.rounded_rectangle([12, 22, 116, 114], radius=14, outline=255, width=W)
    d.rectangle([12, 22, 116, 46], fill=255)
    for x in (38, 90):
        d.line([(x, 8), (x, 30)], fill=255, width=W)
    for i in range(6):
        x, y = 32 + (i % 3) * 32, 64 + (i // 3) * 26
        d.rounded_rectangle([x - 7, y - 7, x + 7, y + 7], radius=3, fill=255)


def sym_pin(d):
    d.polygon([(44, 14), (84, 14), (78, 24), (78, 56), (98, 76), (30, 76), (50, 56), (50, 24)], fill=255)
    d.line([(64, 76), (64, 118)], fill=255, width=W - 1)


def sym_globe(d):
    d.ellipse([12, 12, 116, 116], outline=255, width=W)
    d.ellipse([40, 12, 88, 116], outline=255, width=W - 2)
    d.line([(14, 64), (114, 64)], fill=255, width=W - 2)
    d.arc([22, -10, 106, 46], 30, 150, fill=255, width=W - 3)
    d.arc([22, 82, 106, 138], 210, 330, fill=255, width=W - 3)


def sym_bell(d):
    d.chord([26, 14, 102, 90], 180, 360, fill=255)
    d.polygon([(26, 52), (102, 52), (108, 92), (20, 92)], fill=255)
    d.rounded_rectangle([12, 86, 116, 100], radius=7, fill=255)
    d.ellipse([52, 98, 76, 122], fill=255)
    d.ellipse([56, 4, 72, 20], fill=255)


def sym_hand(d):
    for x, top in ((30, 34), (50, 16), (70, 14), (90, 26)):
        d.rounded_rectangle([x - 8, top, x + 8, 84], radius=8, fill=255)
    d.rounded_rectangle([22, 62, 98, 118], radius=26, fill=255)
    d.line([(26, 86), (8, 58)], fill=255, width=W + 7)


def sym_access(d):
    d.ellipse([52, 6, 76, 30], fill=255)
    d.line([(14, 42), (64, 52), (114, 42)], fill=255, width=W + 2, joint="curve")
    d.line([(64, 50), (64, 78)], fill=255, width=W + 4)
    d.line([(64, 76), (40, 120)], fill=255, width=W + 2)
    d.line([(64, 76), (88, 120)], fill=255, width=W + 2)


def sym_window(d):
    d.rounded_rectangle([10, 18, 118, 110], radius=12, outline=255, width=W)
    d.rectangle([10, 18, 118, 44], fill=255)
    d.rounded_rectangle([10, 18, 118, 50], radius=12, fill=255)
    d.rectangle([14, 44, 114, 48], fill=0)


def sym_text(d):
    d.polygon([(8, 108), (40, 18), (56, 18), (88, 108), (72, 108), (64, 84), (32, 84), (24, 108)], fill=255)
    d.polygon([(37, 70), (59, 70), (48, 36)], fill=0)
    d.ellipse([80, 58, 120, 110], outline=255, width=W)
    d.line([(116, 58), (116, 110)], fill=255, width=W)


def sym_mute(d):
    d.polygon([(10, 46), (34, 46), (64, 18), (64, 110), (34, 82), (10, 82)], fill=255)
    d.line([(80, 46), (116, 82)], fill=255, width=W)
    d.line([(116, 46), (80, 82)], fill=255, width=W)


def sym_screenshot(d):
    for x0, y0, dx, dy in ((10, 18, 1, 1), (118, 18, -1, 1), (10, 110, 1, -1), (118, 110, -1, -1)):
        d.line([(x0, y0 + dy * 26), (x0, y0), (x0 + dx * 26, y0)], fill=255, width=W, joint="curve")
    d.ellipse([46, 46, 82, 82], outline=255, width=W - 1)


def sym_crop(d):
    d.line([(34, 8), (34, 94), (120, 94)], fill=255, width=W + 1, joint="curve")
    d.line([(8, 34), (94, 34), (94, 120)], fill=255, width=W + 1, joint="curve")


def sym_mission(d):
    d.rounded_rectangle([8, 14, 58, 52], radius=7, fill=255)
    d.rounded_rectangle([70, 14, 120, 52], radius=7, fill=255)
    d.rounded_rectangle([26, 64, 102, 114], radius=8, outline=255, width=W - 1)


def sym_widgets(d):
    d.rounded_rectangle([8, 8, 60, 60], radius=12, fill=255)
    d.rounded_rectangle([68, 8, 120, 60], radius=12, outline=255, width=W - 1)
    d.rounded_rectangle([8, 68, 120, 120], radius=12, outline=255, width=W - 1)


def sym_snap(d):
    d.rounded_rectangle([8, 16, 120, 112], radius=12, outline=255, width=W)
    d.rounded_rectangle([18, 26, 58, 102], radius=5, fill=255)
    d.rounded_rectangle([68, 26, 110, 58], radius=5, fill=255)
    d.rounded_rectangle([68, 70, 110, 102], radius=5, fill=255)


def sym_wifi_off(d):
    sym_wifi(d)
    d.line([(18, 18), (110, 110)], fill=0, width=W + 8)
    d.line([(18, 18), (110, 110)], fill=255, width=W)


def sym_copy(d):
    d.rounded_rectangle([40, 10, 116, 86], radius=10, outline=255, width=W)
    d.rounded_rectangle([12, 42, 88, 118], radius=10, fill=0)
    d.rounded_rectangle([12, 42, 88, 118], radius=10, outline=255, width=W)


SYMBOLS = [
    ("SYM_LOGO", sym_logo), ("SYM_SEARCH", sym_search), ("SYM_CONTROL", sym_control), ("SYM_WIFI", sym_wifi),
    ("SYM_HOME", sym_home), ("SYM_DESKTOP", sym_desktop), ("SYM_DOC", sym_doc), ("SYM_APPS", sym_apps),
    ("SYM_DOWNLOAD", sym_download), ("SYM_DISK", sym_disk), ("SYM_TRASH", sym_trash), ("SYM_FOLDER", sym_folder),
    ("SYM_CLOCK", sym_clock), ("SYM_GEAR", sym_gear), ("SYM_USER", sym_user), ("SYM_USERS", sym_users),
    ("SYM_LOCK", sym_lock), ("SYM_POWER", sym_power), ("SYM_RESTART", sym_restart), ("SYM_MOON", sym_moon),
    ("SYM_SUN", sym_sun), ("SYM_DISPLAY", sym_display), ("SYM_DOCK", sym_dock), ("SYM_PHOTO", sym_photo),
    ("SYM_APPEARANCE", sym_appearance), ("SYM_KEYBOARD", sym_keyboard), ("SYM_MOUSE", sym_mouse),
    ("SYM_INFO", sym_info), ("SYM_LEFT", sym_left), ("SYM_RIGHT", sym_right), ("SYM_DOWN", sym_down),
    ("SYM_UP", sym_up), ("SYM_GRID", sym_grid), ("SYM_LIST", sym_list), ("SYM_PLUS", sym_plus),
    ("SYM_MINUS", sym_minus), ("SYM_CHECK", sym_check), ("SYM_CLOSE", sym_close), ("SYM_PENCIL", sym_pencil),
    ("SYM_BRUSH", sym_brush), ("SYM_ERASER", sym_eraser), ("SYM_LINE", sym_line), ("SYM_RECT", sym_rect),
    ("SYM_ELLIPSE", sym_ellipse), ("SYM_BUCKET", sym_bucket), ("SYM_DROPPER", sym_dropper), ("SYM_UNDO", sym_undo),
    ("SYM_TERMINAL", sym_terminal), ("SYM_CHIP", sym_chip), ("SYM_MEMORY", sym_memory),
    ("SYM_NETWORK", sym_network), ("SYM_SIDEBAR", sym_sidebar), ("SYM_STAR", sym_star),
    ("SYM_NEWFOLDER", sym_newfolder), ("SYM_USB", sym_usb), ("SYM_BATTERY", sym_battery), ("SYM_EJECT", sym_eject),
    ("SYM_MUSIC", sym_music), ("SYM_VIDEO", sym_video), ("SYM_PLAY", sym_play), ("SYM_PAUSE", sym_pause),
    ("SYM_PREV", sym_prev), ("SYM_NEXT", sym_next), ("SYM_VOLUME", sym_volume), ("SYM_MORE", sym_more),
    ("SYM_WEATHER", sym_weather), ("SYM_CALENDAR", sym_calendar), ("SYM_PIN", sym_pin), ("SYM_GLOBE", sym_globe),
    ("SYM_BELL", sym_bell), ("SYM_HAND", sym_hand), ("SYM_ACCESS", sym_access), ("SYM_WINDOW", sym_window),
    ("SYM_TEXT", sym_text), ("SYM_MUTE", sym_mute), ("SYM_SCREENSHOT", sym_screenshot), ("SYM_CROP", sym_crop),
    ("SYM_MISSION", sym_mission), ("SYM_WIDGETS", sym_widgets), ("SYM_SNAP", sym_snap), ("SYM_WIFI_OFF", sym_wifi_off),
    ("SYM_COPY", sym_copy),
]


def render_symbol(fn, size):
    big = Image.new("L", (128 * 4, 128 * 4), 0)
    d = ImageDraw.Draw(big)

    class Scaled:
        def __getattr__(self, name):
            orig = getattr(d, name)

            def call(*args, **kw):
                if name in ("arc", "pieslice", "chord"):
                    args = [scale(args[0])] + list(args[1:])
                else:
                    args = [scale(a) for a in args]
                if "width" in kw:
                    kw["width"] = kw["width"] * 4
                if "radius" in kw:
                    kw["radius"] = kw["radius"] * 4
                return orig(*args, **kw)
            return call

    def scale(a):
        if isinstance(a, (list, tuple)):
            return type(a)(scale(v) for v in a)
        if isinstance(a, (int, float)):
            return a * 4
        return a

    fn(Scaled())
    pad = 8 * size // 128
    img = big.resize((size - 2 * pad, size - 2 * pad), Image.LANCZOS)
    out = Image.new("L", (size, size), 0)
    out.paste(img, (pad, pad))
    if size <= 16:
        out = out.point(lambda v: min(255, int(v * 1.25)))
    return list(out.tobytes())


def argb(img):
    raw = img.convert("RGBA").tobytes()
    out = []
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        out.append(a << 24 | r << 16 | g << 8 | b if a else 0)
    return out


def emit_bytes(name, data):
    print(f"static const uint8_t {name}[{len(data)}] = {{")
    for i in range(0, len(data), 24):
        print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    print("};\n")


def main():
    print("/* Generated by tools/mkicons.py. Do not edit. */")
    print("#include <synth/gui.h>\n")
    pack = bytearray()
    entries = []
    for name, fn in ICONS:
        img = fn().resize((SIZE, SIZE), Image.LANCZOS)
        packed = lz4pack.compress(lz4pack.planes(argb(img), SIZE))
        entries.append((name, len(pack), len(packed)))
        pack += packed
    pak = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rootfs", "system", "icons.pak")
    with open(pak, "wb") as f:
        f.write(pack)
    print(f"const uint32_t icon_pack_size = {len(pack)};\n")
    print("const struct icon_entry icon_pack[ICON_COUNT] = {")
    for name, offset, n in entries:
        print(f"    [{name}] = {{{SIZE}, {n}, {offset}}},")
    print("};\n")
    entries = []
    for name, fn in SYMBOLS:
        data = lz4pack.alpha_rows(render_symbol(fn, 16), 16) + lz4pack.alpha_rows(render_symbol(fn, 32), 32)
        packed = lz4pack.compress(data)
        var = name.lower() + "_lz"
        emit_bytes(var, packed)
        entries.append((name, var, len(packed)))
    print("const struct packed_image symbol_pack[SYM_COUNT] = {")
    for name, var, n in entries:
        print(f"    [{name}] = {{16, {n}, {var}}},")
    print("};")


main()
