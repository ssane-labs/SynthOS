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
    def __init__(self, top, bottom, box=BODY):
        self.mask = squircle_mask(box)
        self.img = blank()
        self.img.paste(gradient((S, S), top, bottom), (0, 0), self.mask)

    def blur(self, draw, color, radius, dx=0, dy=0):
        """A shape far behind, out of focus."""
        layer = colored(mask_of(draw), color).filter(ImageFilter.GaussianBlur(radius))
        self.img.alpha_composite(layer, (dx, dy)) if dx or dy else self.img.alpha_composite(layer)
        return self

    def fill(self, draw, top, bottom=None, shadow=0):
        """A crisp shape; shadow lifts it off what is behind."""
        m = mask_of(draw)
        if shadow:
            sh = colored(m, (0, 0, 0, shadow)).filter(ImageFilter.GaussianBlur(7))
            self.img.alpha_composite(sh, (0, 6))
        self.img.alpha_composite(tint_mask(m, top, bottom or top))
        return self

    def glass(self, draw, tint=(255, 255, 255, 70), blur=12, shadow=40):
        """A frosted plate: what lies under it, blurred and lightened, with a faint light from above."""
        m = mask_of(draw)
        if shadow:
            sh = colored(m, (0, 0, 0, shadow)).filter(ImageFilter.GaussianBlur(9))
            self.img.alpha_composite(sh, (0, 8))
        under = self.img.filter(ImageFilter.GaussianBlur(blur))
        plate = Image.composite(under, blank(), m)
        plate.alpha_composite(colored(m, tint))
        light = gradient((S, S), (255, 255, 255, 60), (255, 255, 255, 0))
        light.putalpha(ImageChops.multiply(light.getchannel("A"), m))
        plate.alpha_composite(light)
        edge = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(3)))
        plate.alpha_composite(colored(edge, (255, 255, 255, 70)))
        self.img.alpha_composite(plate)
        return self

    def done(self):
        out = Image.composite(self.img, blank(), self.mask)
        edge = ImageChops.subtract(self.mask, self.mask.filter(ImageFilter.MinFilter(3)))
        out.alpha_composite(colored(edge, (0, 0, 0, 28)))
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


WHITE = (255, 255, 255, 255)


def files_icon():
    return (Icon((96, 186, 255, 255), (20, 98, 230, 255))
            .blur(rr4((40, 60, 200, 150), 24), (10, 60, 170, 200), 14, 0, 10)
            .fill(rr4((52, 74, 120, 104), 12), (200, 230, 255, 255))
            .glass(rr4((52, 92, 204, 190), 20), (220, 238, 255, 110))
            .fill(rr4((106, 136, 150, 146), 5), (30, 120, 235, 255))
            .done())


def terminal_icon():
    return (Icon((70, 72, 82, 255), (20, 20, 24, 255))
            .blur(ellipse4((120, 120, 230, 230)), (60, 220, 120, 170), 26)
            .glass(rr4((44, 56, 212, 200), 18), (40, 44, 52, 120), 10)
            .fill(lambda d: d.line([(72, 98), (104, 124), (72, 150)], fill=255, width=13, joint="curve"), WHITE)
            .fill(rr4((116, 146, 172, 158), 6), (130, 255, 160, 255))
            .done())


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


def settings_icon():
    return (Icon((176, 178, 188, 255), (96, 98, 108, 255))
            .blur(gear_mask(150, 150, 110, 90, 14, 40), (255, 255, 255, 110), 12)
            .glass(ellipse4((54, 54, 202, 202)), (255, 255, 255, 60), 12)
            .fill(gear_mask(128, 128, 62, 50, 12, 22), (250, 250, 252, 255), (210, 212, 220, 255), 60)
            .done())


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


def notepad_icon():
    icon = (Icon((255, 240, 170, 255), (250, 200, 50, 255))
            .blur(rr4((70, 40, 220, 180), 16), (255, 255, 255, 170), 12, 10, -6)
            .glass(rr4((50, 58, 206, 206), 16), (255, 255, 255, 170), 10)
            .fill(rr4((50, 58, 206, 86), 14), (255, 214, 10, 255)))
    for i in range(4):
        y = 118 + i * 22
        icon.fill(rr4((72, y - 3, 184 - (i % 2) * 34, y + 3), 3), (180, 160, 120, 255))
    return icon.done()


def paint_icon():
    icon = Icon((255, 160, 130, 255), (236, 70, 120, 255))
    for (x, y, c) in [(40, 40, (255, 59, 48)), (150, 30, (255, 214, 10)), (170, 150, (52, 199, 89)),
                      (30, 160, (0, 122, 255))]:
        icon.blur(ellipse4((x, y, x + 70, y + 70)), c + (230,), 18)
    icon.glass(ellipse4((46, 64, 210, 200)), (255, 255, 255, 120), 12)
    for (x, y, c) in [(70, 102, (255, 59, 48)), (104, 80, (255, 204, 0)), (146, 82, (52, 199, 89)),
                      (176, 110, (0, 122, 255)), (78, 142, (175, 82, 222))]:
        icon.fill(ellipse4((x, y, x + 28, y + 28)), c + (255,), None, 50)
    return icon.done()


def monitor_icon():
    pts = [(56, 170), (84, 150), (106, 162), (130, 100), (152, 132), (174, 84), (202, 110)]
    return (Icon((44, 52, 58, 255), (12, 16, 18, 255))
            .blur(line4(pts, 30), (80, 245, 140, 200), 14)
            .glass(rr4((40, 56, 216, 200), 20), (255, 255, 255, 26), 8, 0)
            .fill(line4(pts, 11), (150, 255, 180, 255), (60, 220, 120, 255))
            .done())


def browser_icon():
    return (Icon((110, 210, 255, 255), (12, 96, 230, 255))
            .blur(ellipse4((30, 30, 226, 226)), (255, 255, 255, 120), 18)
            .glass(ellipse4((48, 48, 208, 208)), (255, 255, 255, 80), 12)
            .fill(ellipse4((52, 52, 204, 204), True, 7), WHITE)
            .fill(poly4([(128, 128), (176, 80), (140, 140)]), (255, 80, 70, 255), None, 50)
            .fill(poly4([(128, 128), (80, 176), (116, 116)]), WHITE, None, 50)
            .fill(ellipse4((121, 121, 135, 135)), WHITE)
            .done())


def mines_icon():
    cx, cy = 122, 138

    def mine(d):
        for a in range(0, 360, 45):
            r = math.radians(a)
            d.line([(cx, cy), (cx + 52 * math.cos(r), cy + 52 * math.sin(r))], fill=255, width=12)
        d.ellipse([cx - 38, cy - 38, cx + 38, cy + 38], fill=255)
    icon = Icon((160, 230, 120, 255), (40, 150, 70, 255))
    for i in range(3):
        for j in range(3):
            icon.glass(rr4((30 + i * 68, 30 + j * 68, 90 + i * 68, 90 + j * 68), 12), (255, 255, 255, 40), 8, 0)
    return (icon.blur(mine, (0, 0, 0, 120), 10, 0, 8)
            .fill(mine, (40, 44, 52, 255), (16, 18, 22, 255))
            .fill(ellipse4((cx - 22, cy - 22, cx - 4, cy - 4)), (230, 236, 244, 255))
            .fill(rr4((178, 52, 186, 126), 3), WHITE)
            .fill(poly4([(186, 54), (222, 72), (186, 90)]), (255, 59, 48, 255), None, 40)
            .done())


def launchpad_icon():
    colors = [(255, 69, 58), (255, 159, 10), (255, 214, 10), (48, 209, 88), (100, 210, 255), (10, 132, 255),
              (191, 90, 242), (255, 55, 95), (172, 142, 104)]
    icon = Icon((250, 250, 252, 255), (206, 210, 222, 255))
    for i, c in enumerate(colors):
        x, y = 40 + (i % 3) * 62, 40 + (i // 3) * 62
        icon.blur(rr4((x, y, x + 52, y + 52), 14), c + (255,), 10)
    for i, c in enumerate(colors):
        x, y = 60 + (i % 3) * 50, 60 + (i // 3) * 50
        icon.fill(rr4((x, y, x + 38, y + 38), 11), c + (255,), None, 30)
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


def logo_icon():
    icon = (Icon((255, 128, 96, 255), (64, 28, 132, 255))
            .blur(ellipse4((50, 40, 210, 200)), (255, 220, 140, 200), 26))
    m = mirage_mask((S, S), 128, 126, 66)
    sh = colored(m, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(7))
    icon.img.alpha_composite(sh, (0, 6))
    icon.img.alpha_composite(tint_mask(m, (255, 255, 246, 255), (255, 196, 92, 255)))
    return icon.done()


def photos_icon():
    return (Icon((120, 200, 255, 255), (230, 110, 180, 255))
            .blur(ellipse4((130, 40, 210, 120)), (255, 236, 150, 255), 14)
            .blur(poly4([(20, 230), (90, 110), (150, 190), (180, 150), (250, 230)]), (90, 60, 140, 200), 10)
            .glass(poly4([(36, 216), (104, 112), (150, 172), (176, 140), (222, 216)]), (255, 255, 255, 90), 10)
            .fill(ellipse4((150, 64, 190, 104)), (255, 244, 190, 255))
            .done())


def music_icon():
    def note(d):
        d.polygon([(92, 70), (184, 48), (184, 74), (92, 96)], fill=255)
        d.line([(98, 80), (98, 170)], fill=255, width=14)
        d.line([(178, 60), (178, 150)], fill=255, width=14)
        d.ellipse([56, 150, 106, 192], fill=255)
        d.ellipse([136, 130, 186, 172], fill=255)
    return (Icon((255, 120, 130, 255), (176, 32, 120, 255))
            .blur(note, (255, 200, 220, 160), 16, 14, 10)
            .fill(note, WHITE, (255, 226, 236, 255), 70)
            .done())


def video_icon():
    return (Icon((80, 80, 104, 255), (18, 18, 28, 255))
            .blur(ellipse4((30, 40, 140, 150)), (255, 90, 120, 200), 24)
            .blur(ellipse4((120, 110, 230, 220)), (90, 140, 255, 200), 24)
            .glass(rr4((44, 66, 212, 190), 26), (255, 255, 255, 60), 12)
            .fill(poly4([(110, 98), (110, 158), (162, 128)]), WHITE, None, 50)
            .done())


def viewer_icon():
    return (Icon((50, 210, 196, 255), (20, 110, 150, 255))
            .blur(ellipse4((40, 40, 190, 190)), (200, 255, 250, 150), 18)
            .glass(ellipse4((56, 56, 172, 172)), (255, 255, 255, 80), 12)
            .fill(ellipse4((56, 56, 172, 172), True, 14), WHITE)
            .fill(line4([(156, 156), (204, 204)], 24), WHITE, None, 50)
            .done())


def installer_icon():
    return (Icon((120, 156, 255, 255), (70, 40, 200, 255))
            .blur(rr4((40, 120, 216, 230), 26), (20, 10, 90, 160), 14)
            .glass(rr4((50, 146, 206, 206), 20), (255, 255, 255, 110), 10)
            .fill(ellipse4((168, 168, 184, 184)), (90, 110, 240, 255))
            .fill(rr4((116, 40, 140, 120), 8), WHITE, None, 50)
            .fill(poly4([(82, 100), (174, 100), (128, 148)]), WHITE, None, 50)
            .done())


def exe_icon():
    icon = (Icon((130, 150, 182, 255), (70, 86, 120, 255))
            .blur(rr4((70, 40, 230, 170), 18), (255, 255, 255, 120), 14)
            .glass(rr4((46, 62, 210, 196), 16), (255, 255, 255, 150), 10)
            .fill(rr4((46, 62, 210, 92), 16), (225, 228, 236, 255)))
    for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
        icon.fill(ellipse4((60 + i * 22, 70, 74 + i * 22, 84)), c + (255,))
    return (icon.fill(rr4((64, 110, 130, 124), 4), (100, 130, 210, 255))
            .fill(rr4((64, 138, 186, 146), 4), (170, 182, 210, 255))
            .fill(rr4((64, 158, 166, 166), 4), (170, 182, 210, 255))
            .done())


def screenshot_icon():
    def brackets(d):
        for x0, y0, dx, dy in ((52, 60, 1, 1), (204, 60, -1, 1), (52, 196, 1, -1), (204, 196, -1, -1)):
            d.line([(x0, y0 + dy * 34), (x0, y0), (x0 + dx * 34, y0)], fill=255, width=11, joint="curve")
    return (Icon((176, 182, 196, 255), (74, 80, 96, 255))
            .blur(ellipse4((60, 60, 200, 200)), (140, 200, 255, 160), 22)
            .fill(brackets, WHITE, None, 40)
            .glass(rr4((84, 100, 172, 164), 14), (255, 255, 255, 120), 8)
            .fill(rr4((110, 90, 146, 106), 5), (255, 255, 255, 230))
            .fill(ellipse4((110, 114, 146, 150)), (60, 70, 92, 255))
            .fill(ellipse4((119, 123, 137, 141)), (170, 210, 255, 255))
            .done())


def computer_icon():
    return (Icon((236, 236, 242, 255), (176, 176, 184, 255), (20, 60, 236, 196))
            .blur(rr4((40, 80, 216, 180), 20), (255, 255, 255, 180), 12)
            .glass(rr4((34, 74, 222, 150), 16), (255, 255, 255, 70), 8, 0)
            .fill(rr4((44, 150, 212, 153), 1), (150, 150, 158, 255))
            .fill(ellipse4((186, 160, 200, 174)), (52, 199, 89, 255))
            .done())


def usb_icon():
    cx, cy = 128, 130

    def mark(d):
        d.line([(cx, cy - 40), (cx, cy + 40)], fill=255, width=8)
        d.polygon([(cx - 14, cy - 34), (cx + 14, cy - 34), (cx, cy - 56)], fill=255)
        d.line([(cx, cy + 6), (cx - 30, cy - 14), (cx - 30, cy - 26)], fill=255, width=7)
        d.line([(cx, cy + 20), (cx + 30, cy), (cx + 30, cy - 12)], fill=255, width=7)
        d.ellipse([cx - 12, cy + 34, cx + 12, cy + 58], fill=255)
    return (Icon((255, 170, 70, 255), (230, 110, 20, 255), (30, 54, 226, 206))
            .blur(ellipse4((70, 70, 190, 190)), (255, 230, 180, 170), 20)
            .fill(mark, WHITE, None, 50)
            .done())


FOLDER_TOP = (126, 196, 252, 255)
FOLDER_BOTTOM = (83, 166, 240, 255)


def folder_base(emblem=None):
    """A folder of two layers: the back with its tab, and a frosted front plate that blurs it."""
    img = blank()
    back = rounded_mask((24, 52, 232, 214), 18)
    tab = rounded_mask((24, 38, 108, 80), 14)
    back_layer = gradient((S, S), (88, 164, 236, 255), (54, 128, 214, 255))
    img.paste(back_layer, (0, 0), ImageChops.lighter(back, tab))
    sheet = colored(rounded_mask((40, 62, 216, 150), 10), (255, 255, 255, 220))
    img.alpha_composite(sheet.filter(ImageFilter.GaussianBlur(2)))
    front = rounded_mask((24, 80, 232, 220), 18)
    under = img.filter(ImageFilter.GaussianBlur(10))
    plate = Image.composite(under, blank(), front)
    plate.alpha_composite(tint_mask(front, FOLDER_TOP[:3] + (200,), FOLDER_BOTTOM[:3] + (230,)))
    edge = ImageChops.subtract(front, front.filter(ImageFilter.MinFilter(3)))
    plate.alpha_composite(colored(edge, (255, 255, 255, 110)))
    shadow = colored(front, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(6))
    img.alpha_composite(shadow, (0, -2))
    img.alpha_composite(plate)
    if emblem:
        e = blank()
        emblem(e, ImageDraw.Draw(e))
        tint = Image.new("RGBA", (S, S), (40, 110, 196, 255))
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
        rr(d, (72, 96, 184, 196), 8, fill=(120, 190, 250, 255))
        d.polygon([(72, 196), (112, 140), (146, 176), (162, 158), (184, 196)], fill=(70, 170, 90, 255))
        d.ellipse([146, 108, 170, 132], fill=(255, 214, 10, 255))
    return page_base(emblem)


def dll_doc():
    def emblem(img, d):
        gear_shape(d, 128, 148, 48, 38, 10, (130, 136, 150, 255), 16, (255, 255, 255, 255))
    return page_base(emblem)


def dos_doc():
    def emblem(img, d):
        rr(d, (70, 100, 186, 190), 8, fill=(20, 20, 24, 255))
        f = font(MONO_BOLD, 34)
        d.text((80, 126), "C:\\>", font=f, fill=(200, 200, 205, 255))
    return page_base(emblem)


def sx_doc():
    def emblem(img, d):
        rr(d, (78, 110, 178, 180), 14, fill=(150, 80, 230, 255))
        f = font(INTER_BOLD, 44)
        d.text((92, 118), "SX", font=f, fill=(255, 255, 255, 255))
    return page_base(emblem)


def script_doc():
    def emblem(img, d):
        rr(d, (74, 106, 182, 184), 10, fill=(40, 40, 44, 255))
        f = font(MONO_BOLD, 40)
        d.text((88, 118), ">_", font=f, fill=(120, 255, 140, 255))
    return page_base(emblem)


def info_icon():
    def draw(img, d):
        d.ellipse([116, 62, 140, 86], fill=(255, 255, 255, 255))
        rr(d, (114, 102, 142, 194), 10, fill=(255, 255, 255, 255))
    return circle_badge((90, 180, 255, 255), (10, 110, 240, 255), draw)


def power_icon():
    def draw(img, d):
        d.arc([76, 76, 180, 180], 300, 240, fill=(255, 255, 255, 255), width=14)
        d.line([(128, 60), (128, 128)], fill=(255, 255, 255, 255), width=14)
    return circle_badge((255, 105, 97, 255), (220, 40, 40, 255), draw)


def warning_icon():
    img = blank()
    mask = Image.new("L", (S * 4, S * 4), 0)
    md = ImageDraw.Draw(mask)
    md.polygon([(128 * 4, 30 * 4), (232 * 4, 214 * 4), (24 * 4, 214 * 4)], fill=255)
    mask = mask.resize((S, S), Image.LANCZOS)
    img.paste(flat((S, S), (255, 214, 10, 255), (255, 170, 0, 255)), (0, 0), mask)
    d = ImageDraw.Draw(img)
    rr(d, (118, 92, 138, 168), 8, fill=(30, 30, 30, 255))
    d.ellipse([116, 178, 140, 202], fill=(30, 30, 30, 255))
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
    img = blank()
    body = Image.new("L", (S * 4, S * 4), 0)
    ImageDraw.Draw(body).rounded_rectangle([64 * 4, 62 * 4, 192 * 4, 234 * 4], radius=22 * 4, fill=255)
    body = body.resize((S, S), Image.LANCZOS)
    glass = flat((S, S), (236, 244, 252, 170), (188, 204, 222, 170))
    img.paste(glass, (0, 0), body)
    d = ImageDraw.Draw(img)
    if full:
        for (x, y, c) in [(80, 118, (255, 255, 255)), (112, 96, (250, 240, 210)), (134, 130, (220, 236, 255)),
                          (92, 160, (240, 240, 246))]:
            rr(d, (x, y, x + 50, y + 44), 6, fill=c + (235,))
    for i in range(5):
        x = 88 + i * 20
        d.line([(x, 84), (x, 214)], fill=(255, 255, 255, 120), width=6)
    outline = Image.new("RGBA", (S, S), (90, 100, 120, 255))
    outline.putalpha(ImageChops.subtract(body, body.filter(ImageFilter.MinFilter(3))).point(lambda v: v * 90 // 255))
    img.alpha_composite(outline)
    rr(d, (52, 46, 204, 68), 11, fill=(226, 232, 242, 245))
    rr(d, (52, 46, 204, 56), 9, fill=(250, 252, 255, 255))
    rr(d, (106, 34, 150, 48), 6, fill=(214, 222, 234, 255))
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
