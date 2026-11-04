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
    """A sheet with its top right corner folded over, on a soft shadow, the emblem in front."""
    def sheet(d):
        d.rounded_rectangle([52, 20, 204, 236], radius=16, fill=255)
        d.polygon([(158, 16), (210, 16), (210, 68)], fill=0)
    mask = mask_of(sheet)
    img = blank()
    img.alpha_composite(colored(mask, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(6)), (0, 5))
    img.alpha_composite(paint_of((hexc("ffffff"), hexc("eceff4")), mask))
    edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(3)))
    img.alpha_composite(colored(edge, (0, 0, 0, 40)))
    fold = mask_of(lambda d: d.rounded_rectangle([156, 20, 206, 70], radius=6, fill=255,
                                                 corners=(True, False, False, False)))
    fold = ImageChops.multiply(fold, mask_of(poly4([(156, 20), (156, 70), (206, 70)])))
    img.alpha_composite(colored(fold, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(4)), (-2, 3))
    img.alpha_composite(paint_of((hexc("f4f6f9"), hexc("cfd5de")), fold))
    if emblem:
        emblem(img, ImageDraw.Draw(img))
    return img

def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


# ---------------------------------------------------------------- drawing helpers

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
            if "width" in kw:
                kw["width"] = int(round(kw["width"]))
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


def a(c, alpha=255):
    return c + (alpha,)


def top_rounded(box, r):
    return lambda d: d.rounded_rectangle(box, radius=r, fill=255, corners=(True, True, False, False))









































def folder_base(emblem=None):
    """A folder: the back with its tab, a sheet of paper in it, and the lighter front with the emblem pressed in."""
    img = blank()
    back = ImageChops.lighter(rounded_mask((24, 52, 232, 214), 20), rounded_mask((24, 38, 106, 80), 14))
    img.alpha_composite(colored(back, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(5)), (0, 4))
    img.alpha_composite(paint_of((hexc("5aaaf0"), hexc("2f7fd4")), back))
    sheet = rounded_mask((38, 60, 218, 150), 8)
    img.alpha_composite(colored(sheet, (0, 0, 0, 40)).filter(ImageFilter.GaussianBlur(3)), (0, -1))
    img.alpha_composite(paint_of((hexc("ffffff"), hexc("e6edf6")), sheet))
    front = rounded_mask((24, 78, 232, 220), 20)
    img.alpha_composite(colored(front, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(4)), (0, -3))
    img.alpha_composite(paint_of((hexc("a6d8ff"), hexc("62b2f6")), front))
    hi, lo = bands(front, 4)
    img.alpha_composite(colored(ImageChops.multiply(hi, front), (255, 255, 255, 170)))
    img.alpha_composite(colored(ImageChops.multiply(lo, front), (20, 70, 140, 70)))
    if emblem:
        e = blank()
        emblem(e, ImageDraw.Draw(e))
        m = e.getchannel("A")
        img.alpha_composite(colored(ImageChops.offset(m, 0, 2), (255, 255, 255, 110)))
        img.alpha_composite(colored(m, (40, 116, 200, 210)))
    return img


def circle_badge(c_top, c_bottom, draw):
    m = mask_of(ellipse4((30, 30, 226, 226)))
    img = blank()
    img.alpha_composite(colored(m, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(5)), (0, 4))
    img.alpha_composite(paint_of((c_top, c_bottom), m))
    img.alpha_composite(paint_of(((255, 255, 255, 110), (255, 255, 255, 0)), mask_of(gloss((30, 30, 226, 226)))))
    g = blank()
    draw(g, ImageDraw.Draw(g))
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 255))
    sh.putalpha(g.getchannel("A").point(lambda v: v * 70 // 255).filter(ImageFilter.GaussianBlur(5)))
    img.alpha_composite(Image.composite(sh, blank(), m), (0, 3))
    img.alpha_composite(g)
    hi, lo = bands(m, 4)
    img.alpha_composite(colored(ImageChops.multiply(hi, m), (255, 255, 255, 120)))
    img.alpha_composite(colored(ImageChops.multiply(lo, m), (0, 0, 0, 50)))
    return img


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
    img.alpha_composite(colored(mask, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(5)), (0, 4))
    img.alpha_composite(paint_of((hexc("ffd65c"), hexc("f2a516")), mask))
    hi, lo = bands(mask, 4)
    img.alpha_composite(colored(ImageChops.multiply(hi, mask), (255, 255, 255, 140)))
    img.alpha_composite(colored(ImageChops.multiply(lo, mask), (120, 60, 0, 60)))
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
    img.alpha_composite(colored(mask, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(5)), (0, 4))
    img.alpha_composite(paint_of((hexc("c3c8d2"), hexc("838a97")), mask))
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









# ---------------------------------------------------------------- detailed app icons
#
# Drawn like real objects on a colored squircle: a rich vertical gradient lit from above, objects with their
# own gradients, a bevel (light along the top edge, shade along the bottom), soft contact shadows and small
# details. The designs are SynthOS's own.

def hexc(s, alpha=255):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16), alpha)


def radial(cx, cy, r, inner, outer):
    """inner at (cx, cy) fading to outer at distance r."""
    k = 64
    small = Image.new("L", (k, k), 0)
    px = small.load()
    for y in range(k):
        for x in range(k):
            dx, dy = (x + 0.5) * S / k - cx, (y + 0.5) * S / k - cy
            px[x, y] = int(255 * min(1.0, math.hypot(dx, dy) / r))
    ramp = small.resize((S, S), Image.BICUBIC)
    return Image.composite(Image.new("RGBA", (S, S), outer), Image.new("RGBA", (S, S), inner), ramp)


def paint_of(p, m):
    """p is a color, a (top, bottom) pair or an image; returns it cut to the mask m."""
    if isinstance(p, Image.Image):
        img = p.copy()
    elif isinstance(p[0], tuple):
        img = gradient((S, S), p[0], p[1])
    else:
        img = Image.new("RGBA", (S, S), p)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), m))
    return img


def bands(m, k):
    """Light along the top inside edge and shade along the bottom inside edge of a shape."""
    top = ImageChops.subtract(m, ImageChops.offset(m, 0, k))
    bottom = ImageChops.subtract(m, ImageChops.offset(m, 0, -k))
    return top.filter(ImageFilter.GaussianBlur(k / 2)), bottom.filter(ImageFilter.GaussianBlur(k / 2))


def rotated(draw, angle, cx=128, cy=128):
    m = mask_of(draw)
    return m.rotate(angle, resample=Image.BICUBIC, center=(cx, cy))


class Art:
    def __init__(self, top, bottom, box=BODY, glow=70):
        self.box = box
        self.mask = squircle_mask(box)
        self.img = gradient((S, S), top, bottom)
        if glow:
            x0, y0, x1, y1 = box
            self.img.alpha_composite(radial((x0 + x1) / 2, y0, (y1 - y0) * 0.9, (255, 255, 255, glow),
                                            (255, 255, 255, 0)))

    def m(self, shape):
        return shape if isinstance(shape, Image.Image) else mask_of(shape)

    def shadow(self, shape, alpha=70, blur=6, dy=5, dx=0):
        sh = colored(self.m(shape), (0, 0, 0, alpha)).filter(ImageFilter.GaussianBlur(blur))
        self.img.alpha_composite(sh, (dx, dy))
        return self

    def put(self, shape, paint, shadow=0, bevel=0, light=110, shade=60, stroke=None, blur=0):
        m = self.m(shape)
        if shadow:
            self.shadow(m, shadow, 4 + shadow // 20, 3 + shadow // 25)
        layer = paint_of(paint, m)
        if blur:
            layer = layer.filter(ImageFilter.GaussianBlur(blur))
        self.img.alpha_composite(layer)
        if bevel:
            hi, lo = bands(m, bevel)
            self.img.alpha_composite(colored(ImageChops.multiply(hi, m), (255, 255, 255, light)))
            self.img.alpha_composite(colored(ImageChops.multiply(lo, m), (0, 0, 0, shade)))
        if stroke:
            edge = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(3)))
            self.img.alpha_composite(colored(edge, stroke))
        return self

    def clip(self, shape, paint, within):
        """paint the shape only where the mask within is."""
        m = ImageChops.multiply(self.m(shape), self.m(within))
        self.img.alpha_composite(paint_of(paint, m))
        return self

    def done(self, shadow=60):
        out = blank()
        if shadow:
            sh = colored(self.mask, (0, 0, 0, shadow)).filter(ImageFilter.GaussianBlur(5))
            out.alpha_composite(sh, (0, 4))
        out.alpha_composite(Image.composite(self.img, blank(), self.mask))
        hi, lo = bands(self.mask, 4)
        out.alpha_composite(colored(ImageChops.multiply(hi, self.mask), (255, 255, 255, 90)))
        out.alpha_composite(colored(ImageChops.multiply(lo, self.mask), (0, 0, 0, 50)))
        return out


def gloss(box):
    """The soft reflection on the upper half of a round or glassy object."""
    x0, y0, x1, y1 = box
    return ellipse4((x0 + (x1 - x0) * 0.12, y0 + (y1 - y0) * 0.04, x1 - (x1 - x0) * 0.12, y0 + (y1 - y0) * 0.5))


def files_art():
    """A file drawer pulled open, colored folder tabs standing in it."""
    art = Art(hexc("7cc8ff"), hexc("1f6fe0"))
    art.put(rr4((50, 96, 206, 214), 18), (hexc("e9eef6"), hexc("b9c4d4")), 70, 3)
    for i, (c1, c2) in enumerate((("ffd45a", "f2a91c"), ("7fd98c", "35a853"), ("ff8a80", "e2483d"))):
        x = 66 + i * 40
        art.put(top_rounded((x, 62 + i * 6, x + 64, 140), 9), (hexc(c1), hexc(c2)), 40, 3)
        art.put(rr4((x + 6, 78 + i * 6, x + 58, 140), 4), (hexc("ffffff"), hexc("e8e8e8")), 0)
    art.put(rr4((44, 124, 212, 220), 20), (hexc("ffffff"), hexc("d6dde8")), 60, 4)
    art.put(rr4((100, 150, 156, 166), 8), (hexc("6d7a8c"), hexc("9aa6b6")), 0, 2, 40, 80)
    art.put(rr4((104, 153, 152, 157), 2), (255, 255, 255, 120))
    return art.done()


def terminal_art():
    art = Art(hexc("eceef2"), hexc("a9adb6"))
    art.put(rr4((40, 48, 216, 208), 22), (hexc("50545c"), hexc("2a2c31")), 60, 3)
    art.put(rr4((46, 66, 210, 202), 16), (hexc("23262c"), hexc("0b0c0e")))
    for i, c in enumerate(("ff5f57", "febc2e", "28c840")):
        art.put(ellipse4((54 + i * 15, 53, 64 + i * 15, 63)), hexc(c))
    art.put(line4([(68, 98), (98, 122), (68, 146)], 13), (hexc("f4f5f7"), hexc("c9ccd2")))
    art.put(rr4((108, 140, 156, 153), 4), (hexc("7cf29a"), hexc("2fbf5a")))
    art.put(rr4((68, 172, 188, 178), 3), (255, 255, 255, 40))
    art.put(rr4((50, 70, 206, 120), 14), ((255, 255, 255, 28), (255, 255, 255, 0)))
    return art.done()


def settings_art():
    art = Art(hexc("e6e8ec"), hexc("8d929c"))
    art.put(ellipse4((40, 40, 216, 216)), (hexc("7a7f89"), hexc("3f434a")), 50, 3, 70, 60)
    art.put(gear_mask(128, 128, 82, 68, 16, 0), radial(110, 96, 110, hexc("ffffff"), hexc("9ca1ab")), 70, 3)
    art.put(ellipse4((76, 76, 180, 180)), (hexc("5e636c"), hexc("2c2f35")), 0, 3, 60, 40)
    art.put(gear_mask(128, 128, 46, 38, 10, 0), radial(116, 108, 64, hexc("f4f5f7"), hexc("8a8f98")), 40, 2)
    art.put(ellipse4((110, 110, 146, 146)), (hexc("4a4e56"), hexc("6c717a")), 0, 2)
    art.put(ellipse4((120, 120, 136, 136)), (hexc("d8dbe0"), hexc("a4a9b2")))
    return art.done()


def calc_art():
    art = Art(hexc("56585f"), hexc("1d1e22"))
    art.put(rr4((46, 44, 210, 92), 12), (hexc("2b3a34"), hexc("1c2622")), 0, 2, 30, 60)
    art.put(text4("42", INTER_BOLD, 34, (156, 46)), (hexc("e8fff1"), hexc("a9d9bc")))
    for row in range(3):
        for col in range(4):
            x, y = 48 + col * 41, 106 + row * 36
            if col == 3:
                p = (hexc("ffb340"), hexc("f07c00"))
            elif row == 0:
                p = (hexc("c7c9ce"), hexc("9a9da4"))
            else:
                p = (hexc("6b6e75"), hexc("45474d"))
            art.put(rr4((x, y, x + 34, y + 29), 10), p, 40, 2, 90, 60)
    art.put(rr4((120, 115, 140, 119), 2), hexc("ffffff"))
    art.put(rr4((128, 107, 132, 127), 2), hexc("ffffff"))
    return art.done()


def notepad_art():
    art = Art(hexc("fffdf6"), hexc("efe9d8"), glow=0)
    art.put(top_rounded((24, 24, 232, 84), 44), (hexc("ffd95a"), hexc("f3b313")), 0, 3, 90, 40)
    art.put(rr4((24, 82, 232, 88), 0), (0, 0, 0, 40), blur=2)
    for i in range(6):
        art.put(ellipse4((52 + i * 28, 60, 62 + i * 28, 70)), (hexc("b88a10"), hexc("e0ae2a")))
    for i in range(6):
        y = 110 + i * 19
        art.put(rr4((44, y, 212, y + 2.4), 1.2), hexc("c9d6ea"))
    art.put(rr4((70, 92, 72.5, 224), 1), hexc("f1a3a3"))
    pencil = rotated(rr4((78, 120, 238, 142), 4), 38, 150, 140)
    art.shadow(pencil, 70, 5, 6)
    art.clip(rotated(rr4((78, 120, 238, 142), 4), 38, 150, 140), (hexc("ffcc33"), hexc("e99a00")), pencil)
    art.clip(rotated(rr4((206, 120, 238, 142), 4), 38, 150, 140), (hexc("f7a6b4"), hexc("e07a8c")), pencil)
    art.clip(rotated(rr4((196, 120, 208, 142), 0), 38, 150, 140), (hexc("d9dce2"), hexc("9aa0aa")), pencil)
    tip = rotated(poly4([(78, 120), (78, 142), (52, 131)]), 38, 150, 140)
    art.put(tip, (hexc("f3d3a1"), hexc("d9a868")))
    art.put(rotated(poly4([(62, 126), (62, 136), (52, 131)]), 38, 150, 140), hexc("3a3a3e"))
    art.clip(rotated(rr4((78, 120, 196, 126), 0), 38, 150, 140), (255, 255, 255, 90), pencil)
    return art.done()


def paint_art():
    art = Art(hexc("8fd3ff"), hexc("3a7bd5"))

    def palette(d):
        d.ellipse([36, 58, 220, 214], fill=255)
        d.ellipse([170, 168, 230, 228], fill=0)
        d.ellipse([140, 150, 166, 176], fill=0)
    art.put(palette, (hexc("f2c98f"), hexc("c98d4f")), 70, 4)
    for (x, y, c1, c2) in ((64, 104, "ff6b5e", "d6372b"), (98, 76, "ffd84a", "f0a500"), (140, 80, "6fe08b", "26a64a"),
                           (178, 108, "6db8ff", "1f6fe0"), (72, 150, "b98cff", "7a4be0")):
        art.put(ellipse4((x, y, x + 32, y + 28)), (hexc(c1), hexc(c2)), 30, 2)
        art.put(ellipse4((x + 7, y + 5, x + 17, y + 12)), (255, 255, 255, 170))
    k = (-40, 168, 170)
    brush = rotated(rr4((104, 164, 222, 176), 6), *k)
    art.shadow(brush, 60, 4, 5)
    art.clip(rotated(rr4((104, 164, 186, 176), 6), *k), (hexc("ff9a3c"), hexc("d0631a")), brush)
    art.clip(rotated(rr4((184, 162, 206, 178), 3), *k), (hexc("eef0f3"), hexc("9aa0aa")), brush)
    art.put(rotated(poly4([(204, 161), (236, 170), (204, 179)]), *k), (hexc("4a4e57"), hexc("1d1f24")))
    return art.done()


def monitor_art():
    art = Art(hexc("3a3e46"), hexc("121316"))
    art.put(rr4((40, 50, 216, 206), 18), (hexc("13161b"), hexc("07080a")), 40, 2, 30, 0)
    for i in range(1, 4):
        art.put(rr4((48, 50 + i * 39, 208, 51 + i * 39), 0), (120, 255, 160, 30))
        art.put(rr4((40 + i * 44, 58, 41 + i * 44, 198), 0), (120, 255, 160, 30))
    pts = [(48, 170), (74, 150), (98, 160), (124, 108), (148, 134), (172, 84), (208, 104)]

    def area(d):
        d.polygon(pts + [(208, 198), (48, 198)], fill=255)
    art.put(area, ((90, 255, 140, 120), (90, 255, 140, 0)))
    art.put(line4(pts, 9), (hexc("b6ffcb"), hexc("37d06a")))
    art.put(line4(pts, 22), (80, 255, 140, 60), blur=6)
    art.put(ellipse4((166, 78, 178, 90)), hexc("ffffff"))
    return art.done()


def mines_art():
    art = Art(hexc("a9b6c6"), hexc("5d6b7e"))
    for i in range(3):
        for j in range(3):
            x, y = 38 + i * 62, 38 + j * 62
            art.put(rr4((x, y, x + 56, y + 56), 12), (hexc("e8edf3"), hexc("b7c2cf")), 30, 3)
    cx, cy = 128, 128

    def mine(d):
        for ang in range(0, 360, 45):
            r = math.radians(ang)
            d.line([(cx, cy), (cx + 40 * math.cos(r), cy + 40 * math.sin(r))], fill=255, width=9)
        d.ellipse([cx - 28, cy - 28, cx + 28, cy + 28], fill=255)
    art.put(mine, radial(118, 116, 50, hexc("5a5e66"), hexc("101114")), 70)
    art.put(ellipse4((cx - 18, cy - 18, cx - 4, cy - 4)), (255, 255, 255, 220), blur=1)
    art.put(rr4((182, 44, 187, 84), 2), hexc("3a3d44"))
    art.put(poly4([(187, 44), (214, 54), (187, 66)]), (hexc("ff5a4f"), hexc("c62a20")), 30)
    art.put(text4("1", INTER_BOLD, 34, (54, 160)), hexc("2f7ae5"))
    art.put(text4("2", INTER_BOLD, 34, (178, 160)), hexc("2aa356"))
    return art.done()


def browser_art():
    """A globe with an orbit around it."""
    art = Art(hexc("5bd0ff"), hexc("0a58d6"))
    g = ellipse4((54, 54, 202, 202))
    art.put(g, radial(104, 92, 160, hexc("9fe7ff"), hexc("0b62c9")), 80)
    land = mask_of(lambda d: (d.polygon([(70, 100), (98, 76), (126, 84), (118, 112), (92, 124), (80, 150), (66, 136)],
                                        fill=255),
                              d.polygon([(132, 128), (168, 116), (190, 136), (176, 170), (148, 186), (138, 160)],
                                        fill=255),
                              d.ellipse([140, 74, 170, 96], fill=255)))
    art.clip(land.filter(ImageFilter.GaussianBlur(1.2)), (hexc("8ff0a8"), hexc("2fae5c")), g)
    for k in (0.42, 0.75):
        art.clip(ellipse4((128 - 74 * k, 54, 128 + 74 * k, 202), True, 2.5), (255, 255, 255, 90), g)
    for y in (90, 128, 166):
        art.clip(rr4((40, y - 1.2, 216, y + 1.2), 1), (255, 255, 255, 80), g)
    art.put(gloss((54, 54, 202, 202)), ((255, 255, 255, 120), (255, 255, 255, 0)))
    orbit = rotated(ellipse4((26, 106, 230, 150), True, 7), -18)
    front = ImageChops.multiply(orbit, mask_of(rr4((0, 128, 256, 256), 0)).rotate(-18, center=(128, 128)))
    back = ImageChops.subtract(orbit, front)
    art.clip(back, (255, 255, 255, 110), back)
    art.put(front, (hexc("ffffff"), hexc("dcefff")), 40)
    art.put(ellipse4((196, 82, 216, 102)), radial(202, 88, 14, hexc("ffffff"), hexc("ffd45a")), 40)
    return art.done()


def photos_art():
    """Two instant photos, the front one a sunny lake view."""
    art = Art(hexc("ffffff"), hexc("e3e6ec"))
    back = rotated(rr4((60, 52, 200, 204), 8), 12)
    art.put(back, (hexc("ffffff"), hexc("eceef2")), 60, 2)
    art.clip(rotated(rr4((72, 64, 188, 166), 3), 12), (hexc("ffb36b"), hexc("ff6f91")), back)
    front = rotated(rr4((52, 58, 192, 212), 8), -8)
    art.put(front, (hexc("ffffff"), hexc("f0f1f4")), 80, 2)
    pic = rotated(rr4((64, 70, 180, 172), 3), -8)
    art.clip(pic, (hexc("7fd1ff"), hexc("d9f2ff")), front)
    art.clip(rotated(ellipse4((136, 82, 162, 108)), -8), radial(146, 92, 18, hexc("fff6c2"), hexc("ffc531")), pic)
    art.clip(rotated(poly4([(60, 160), (98, 108), (124, 138), (146, 116), (186, 160)]), -8),
             (hexc("5ccf7b"), hexc("23894a")), pic)
    art.clip(rotated(rr4((60, 150, 186, 176), 0), -8), (hexc("4aa8e8"), hexc("2a74c8")), pic)
    art.clip(rotated(rr4((70, 156, 120, 159), 1), -8), (255, 255, 255, 140), pic)
    return art.done()


def music_art():
    """A vinyl record with a warm label and a note in front."""
    art = Art(hexc("ff8a5c"), hexc("d92e6b"))
    disc = ellipse4((44, 44, 212, 212))
    art.put(disc, radial(110, 100, 130, hexc("3a3b40"), hexc("0b0b0d")), 80)
    for r in range(78, 40, -7):
        art.clip(ellipse4((128 - r, 128 - r, 128 + r, 128 + r), True, 1.2), (255, 255, 255, 26), disc)

    def sheen(d):
        d.pieslice([44, 44, 212, 212], 200, 245, fill=255)
        d.pieslice([44, 44, 212, 212], 20, 65, fill=255)
    art.clip(lambda d: sheen(d), (255, 255, 255, 46), disc)
    art.put(ellipse4((96, 96, 160, 160)), (hexc("ffd36b"), hexc("ff8c32")), 0, 2)
    art.put(ellipse4((122, 122, 134, 134)), hexc("1a1a1d"))

    def note(d):
        d.polygon([(162, 94), (214, 82), (214, 96), (162, 108)], fill=255)
        d.line([(166, 100), (166, 176)], fill=255, width=9)
        d.line([(210, 88), (210, 164)], fill=255, width=9)
        d.ellipse([140, 166, 172, 190], fill=255)
        d.ellipse([184, 154, 216, 178], fill=255)
    art.put(note, (hexc("ffffff"), hexc("f1e4ea")), 80)
    return art.done()


def video_art():
    """A clapperboard with a play mark."""
    art = Art(hexc("7f7bff"), hexc("3a2fc2"))
    art.put(rr4((46, 96, 210, 206), 14), (hexc("2e3038"), hexc("121317")), 80, 3, 60)
    top = rotated(rr4((46, 66, 210, 96), 6), -10, 50, 96)
    art.put(top, (hexc("2e3038"), hexc("1a1b20")), 50, 2)
    for i in range(5):
        x = 52 + i * 34
        art.clip(rotated(poly4([(x, 66), (x + 18, 66), (x + 6, 96), (x - 12, 96)]), -10, 50, 96), hexc("f4f5f7"), top)
    art.put(rr4((46, 96, 210, 104), 0), (hexc("f4f5f7"), hexc("c9ccd2")))
    art.put(poly4([(112, 126), (112, 182), (160, 154)]), (hexc("ffffff"), hexc("d9dbe2")), 40)
    return art.done()


def viewer_art():
    """A picture under a magnifying glass."""
    art = Art(hexc("f4f7fb"), hexc("cfd7e2"))
    card = rr4((44, 56, 186, 186), 12)
    art.put(card, (hexc("ffffff"), hexc("eef1f5")), 60, 2)
    art.clip(rr4((56, 68, 174, 174), 6), (hexc("8fd6ff"), hexc("e6f6ff")), card)
    art.clip(poly4([(56, 174), (96, 116), (122, 144), (142, 124), (174, 174)]), (hexc("7bd88f"), hexc("2f9b57")), card)
    art.put(line4([(168, 168), (208, 208)], 22), (hexc("4b4f58"), hexc("1d1f24")), 60)
    lens = ellipse4((104, 104, 188, 188))
    art.put(lens, (hexc("e9f6ff", 120), hexc("bfe3ff", 90)), 50)
    art.put(ellipse4((104, 104, 188, 188), True, 10), (hexc("e3e6ea"), hexc("8d939d")), 0, 2)
    art.put(gloss((112, 112, 180, 180)), ((255, 255, 255, 150), (255, 255, 255, 0)))
    return art.done()


def installer_art():
    """A drive with a download arrow over it."""
    art = Art(hexc("eef0f4"), hexc("aeb4bf"))
    art.put(rr4((46, 140, 210, 206), 16), (hexc("f6f7f9"), hexc("b8bdc6")), 70, 3)
    art.put(rr4((60, 182, 150, 186), 2), (0, 0, 0, 40))
    art.put(ellipse4((176, 176, 190, 190)), radial(181, 181, 8, hexc("b9ffcc"), hexc("27c25a")))
    art.put(rr4((112, 40, 144, 112), 10), (hexc("6cc0ff"), hexc("1a6fe0")), 60, 2)
    art.put(poly4([(82, 100), (174, 100), (128, 148)]), (hexc("4fb0ff"), hexc("165fd0")), 50, 2)
    return art.done()


def screenshot_art():
    art = Art(hexc("e8eaee"), hexc("9aa0aa"))

    def brackets(d):
        for x0, y0, dx, dy in ((48, 56, 1, 1), (208, 56, -1, 1), (48, 200, 1, -1), (208, 200, -1, -1)):
            d.line([(x0, y0 + dy * 34), (x0, y0), (x0 + dx * 34, y0)], fill=255, width=9, joint="curve")
    art.put(brackets, (hexc("ffffff"), hexc("eceef2")), 50)
    art.put(rr4((78, 96, 178, 170), 16), (hexc("4a4e57"), hexc("1d1f24")), 70, 3)
    art.put(rr4((106, 84, 150, 100), 6), (hexc("4a4e57"), hexc("33363d")))
    art.put(ellipse4((106, 108, 150, 152)), radial(122, 120, 30, hexc("6c9bd6"), hexc("0e1b2e")), 0, 2)
    art.put(ellipse4((118, 116, 130, 128)), (255, 255, 255, 190))
    art.put(ellipse4((156, 106, 166, 116)), hexc("ff9f2e"))
    return art.done()


def store_art():
    """A shopping bag with a colored tag."""
    art = Art(hexc("6fd0ff"), hexc("1660e6"))
    art.put(lambda d: d.arc((92, 42, 164, 120), 180, 360, fill=255, width=11), (hexc("ffffff"), hexc("cfe3ff")), 40)
    bag = poly4([(58, 84), (198, 84), (210, 214), (46, 214)])
    art.put(bag, (hexc("ffffff"), hexc("dfe8f5")), 80, 4)
    art.put(rr4((58, 84, 198, 96), 0), (0, 0, 0, 25))
    art.put(ellipse4((88, 98, 102, 112)), hexc("8da3c2"))
    art.put(ellipse4((154, 98, 168, 112)), hexc("8da3c2"))
    star = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        r = 34 if i % 2 == 0 else 15
        star.append((128 + r * math.cos(ang), 160 + r * math.sin(ang)))
    art.put(poly4(star), (hexc("ffd45a"), hexc("ff9a1f")), 40, 2)
    return art.done()


def code_art():
    """An editor window with colored code and a tag in front."""
    art = Art(hexc("3a3d52"), hexc("15161f"))
    art.put(rr4((40, 48, 216, 196), 16), (hexc("262838"), hexc("181925")), 60, 2, 40)
    for i, c in enumerate(("ff5f57", "febc2e", "28c840")):
        art.put(ellipse4((52 + i * 14, 58, 61 + i * 14, 67)), hexc(c))
    lines = [(56, 60, "ff7ab6"), (56, 110, "7cc4ff"), (72, 90, "ffd166"), (72, 130, "8ef0a5"), (56, 80, "b69cff"),
             (72, 104, "7cc4ff")]
    for i, (x, w, c) in enumerate(lines):
        y = 82 + i * 17
        art.put(rr4((x, y, x + w, y + 7), 3.5), hexc(c))
    art.put(rr4((118, 150, 226, 214), 18), (hexc("8b7bff"), hexc("5236e0")), 70, 3)
    art.put(line4([(150, 166), (136, 182), (150, 198)], 8), hexc("ffffff"))
    art.put(line4([(194, 166), (208, 182), (194, 198)], 8), hexc("ffffff"))
    art.put(line4([(178, 162), (166, 202)], 7), hexc("e5dcff"))
    return art.done()


def games_art():
    """A glossy game controller."""
    art = Art(hexc("ff8a73"), hexc("e0356b"))

    def body(d):
        d.rounded_rectangle((38, 90, 218, 172), radius=40, fill=255)
        d.ellipse((38, 118, 112, 206), fill=255)
        d.ellipse((144, 118, 218, 206), fill=255)
    art.put(body, (hexc("ffffff"), hexc("d9dde6")), 90, 4)
    art.put(rr4((66, 124, 112, 140), 5), (hexc("4a4e57"), hexc("23252a")))
    art.put(rr4((81, 109, 97, 155), 5), (hexc("4a4e57"), hexc("23252a")))
    for cx, cy, c1, c2 in ((172, 116, "ff6b5e", "d6372b"), (192, 136, "6db8ff", "1f6fe0"), (152, 136, "ffd84a", "f0a500"),
                           (172, 156, "6fe08b", "26a64a")):
        art.put(ellipse4((cx - 10, cy - 10, cx + 10, cy + 10)), (hexc(c1), hexc(c2)), 30, 2)
    art.put(gloss((60, 92, 196, 150)), ((255, 255, 255, 120), (255, 255, 255, 0)))
    return art.done()


def launchpad_art():
    art = Art(hexc("4a4e58"), hexc("1a1b20"))
    tones = [("ff6b5e", "d6372b"), ("ffd84a", "f0a500"), ("6fe08b", "26a64a"), ("6db8ff", "1f6fe0"),
             ("b98cff", "7a4be0"), ("ff9ec4", "e0568e"), ("7ae7e0", "1aa8a0"), ("ffb36b", "f07c00"),
             ("c7c9ce", "8d9098")]
    for i, (c1, c2) in enumerate(tones):
        x, y = 48 + (i % 3) * 56, 48 + (i // 3) * 56
        art.put(rr4((x, y, x + 46, y + 46), 13), (hexc(c1), hexc(c2)), 40, 3)
    return art.done()


def logo_art():
    art = Art(hexc("ffa46b"), hexc("6b3a8f"))
    m = mirage_mask((S, S), 128, 124, 66)
    art.put(m, (hexc("fffaf0"), hexc("ffd59a")), 60)
    return art.done()


def exe_art():
    art = Art(hexc("e6e9ee"), hexc("9ca3ae"))
    art.put(rr4((44, 58, 212, 200), 16), (hexc("ffffff"), hexc("eef1f5")), 70, 2)
    art.put(top_rounded((44, 58, 212, 88), 16), (hexc("e9ecf1"), hexc("d2d7de")))
    for i, c in enumerate(("ff5f57", "febc2e", "28c840")):
        art.put(ellipse4((56 + i * 16, 67, 67 + i * 16, 78)), hexc(c))
    art.put(rr4((60, 104, 120, 184), 8), (hexc("6cc0ff"), hexc("1a6fe0")))
    for i, w in enumerate((70, 56, 64, 44)):
        art.put(rr4((132, 108 + i * 20, 132 + w, 116 + i * 20), 4), hexc("c9ced6"))
    return art.done()


def computer_art():
    """An internal disk: brushed aluminium with a light."""
    img = blank()
    body = mask_of(rr4((24, 70, 232, 186), 22))
    img.alpha_composite(colored(body, (0, 0, 0, 70)).filter(ImageFilter.GaussianBlur(6)), (0, 6))
    img.alpha_composite(paint_of((hexc("f7f8fa"), hexc("b6bbc4")), body))
    streaks = Image.new("L", (1, S))
    for y in range(S):
        streaks.putpixel((0, y), int(128 + 14 * math.sin(y * 1.7) + 9 * math.sin(y * 4.3 + 1)))
    streaks = streaks.resize((S, S))
    img.alpha_composite(paint_of(Image.merge("RGBA", (streaks, streaks, streaks, Image.new("L", (S, S), 28))), body))
    hi, lo = bands(body, 4)
    img.alpha_composite(colored(ImageChops.multiply(hi, body), (255, 255, 255, 200)))
    img.alpha_composite(colored(ImageChops.multiply(lo, body), (0, 0, 0, 70)))
    img.alpha_composite(paint_of((0, 0, 0, 60), mask_of(rr4((40, 150, 216, 152), 1))))
    img.alpha_composite(paint_of((255, 255, 255, 160), mask_of(rr4((40, 152, 216, 154), 1))))
    img.alpha_composite(paint_of(radial(198, 168, 10, hexc("c4ffd4"), hexc("1fb455")), mask_of(ellipse4((190, 160, 206, 176)))))
    return img


def usb_art():
    """An orange external drive with the USB mark."""
    art = Art(hexc("ffb35c"), hexc("e8701a"))
    cx, cy = 128, 130

    def mark(d):
        d.line([(cx, cy - 40), (cx, cy + 40)], fill=255, width=9)
        d.polygon([(cx - 15, cy - 34), (cx + 15, cy - 34), (cx, cy - 58)], fill=255)
        d.line([(cx, cy + 6), (cx - 30, cy - 14), (cx - 30, cy - 26)], fill=255, width=8, joint="curve")
        d.line([(cx, cy + 20), (cx + 30, cy), (cx + 30, cy - 12)], fill=255, width=8, joint="curve")
        d.ellipse([cx - 13, cy + 33, cx + 13, cy + 59], fill=255)
        d.ellipse([cx - 38, cy - 36, cx - 22, cy - 20], fill=255)
        d.rectangle([cx + 22, cy - 26, cx + 38, cy - 10], fill=255)
    art.put(mark, (hexc("ffffff"), hexc("ffe9d2")), 70, 2, 60, 30)
    return art.done()


ICONS = [
    ("ICON_FOLDER", folder_icon), ("ICON_FILE", blank_doc), ("ICON_TEXT", text_doc), ("ICON_APP", exe_art),
    ("ICON_DLL", dll_doc), ("ICON_IMAGE", image_doc), ("ICON_TERMINAL", terminal_art),
    ("ICON_SETTINGS", settings_art), ("ICON_CALC", calc_art), ("ICON_NOTEPAD", notepad_art),
    ("ICON_PAINT", paint_art), ("ICON_COMPUTER", computer_art), ("ICON_HOME", home_folder_icon),
    ("ICON_MONITOR", monitor_art), ("ICON_MINES", mines_art), ("ICON_DOS", dos_doc), ("ICON_SX", sx_doc),
    ("ICON_SCRIPT", script_doc), ("ICON_POWER", power_icon), ("ICON_LOGO", logo_art), ("ICON_INFO", info_icon),
    ("ICON_FILES", files_art), ("ICON_LAUNCHPAD", launchpad_art), ("ICON_WARNING", warning_icon),
    ("ICON_USER", user_icon), ("ICON_TRASH", trash_icon), ("ICON_TRASH_FULL", trash_full_icon),
    ("ICON_USB", usb_art), ("ICON_FOLDER_APPS", apps_folder_icon), ("ICON_FOLDER_DOCS", docs_folder_icon),
    ("ICON_FOLDER_DESKTOP", desktop_folder_icon), ("ICON_FOLDER_DOWNLOADS", downloads_folder_icon),
    ("ICON_BROWSER", browser_art), ("ICON_PHOTOS", photos_art), ("ICON_MUSIC", music_art),
    ("ICON_VIDEO", video_art), ("ICON_VIEWER", viewer_art), ("ICON_INSTALLER", installer_art),
    ("ICON_SCREENSHOT", screenshot_art), ("ICON_STORE", store_art), ("ICON_CODE", code_art),
    ("ICON_GAMES", games_art),
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
