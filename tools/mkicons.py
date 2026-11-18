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


def leaf_mask(box):
    """The SynthOS tile: wide round corners at the top and bottom left, a tight one at the bottom right."""
    x0, y0, x1, y1 = box
    w = x1 - x0
    big = Image.new("L", (S * 4, S * 4), 0)
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([x0 * 4, y0 * 4, x1 * 4, y1 * 4], radius=int(w * 0.31 * 4), fill=255)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    d.rounded_rectangle([cx * 4, cy * 4, x1 * 4, y1 * 4], radius=int(w * 0.11 * 4), fill=255)
    return big.resize((S, S), Image.LANCZOS)


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


_DIAG = []


def diag_light():
    """Where light catches a glass edge: strong at the top left, weaker at the bottom right."""
    if not _DIAG:
        k = 64
        small = Image.new("L", (k, k), 0)
        px = small.load()
        for y in range(k):
            for x in range(k):
                v = (x + y + 1) / (2 * k)
                tl = max(0.0, 1 - 2.2 * v) ** 0.7
                br = max(0.0, 2.2 * v - 1.2) ** 0.7 * 0.6
                px[x, y] = int(255 * min(1.0, max(tl, br) + 0.12))
        _DIAG.append(small.resize((S, S), Image.BICUBIC))
    return _DIAG[0]


def specular(m, width=3):
    """A thin bright band just inside the edge of a shape, lit from the top left like the rim of thick glass."""
    edge = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(width * 2 + 1))).filter(ImageFilter.GaussianBlur(0.7))
    return ImageChops.multiply(edge, diag_light())


def deepen(c, base, k):
    return tuple(int(c[i] * (1 - k) + base[i] * k) for i in range(3)) + (c[3] if len(c) > 3 else 255,)


def rotated(draw, angle, cx=128, cy=128):
    m = mask_of(draw)
    return m.rotate(angle, resample=Image.BICUBIC, center=(cx, cy))


class Tile:
    """An app icon as the desktop stores it: the squircle's background colors (light and dark) and everything drawn
    over it as one transparent layer, so the kernel paints the background in either mode and lays the layer on top."""

    def __init__(self, fg, top, bottom, glow, box):
        self.fg, self.top, self.bottom, self.glow, self.box = fg, top, bottom, glow, box
        self.dtop, self.dbottom = deepen(top, (44, 44, 50), 0.80), deepen(bottom, (14, 14, 17), 0.88)
        self.dglow = glow // 4

    def background(self, dark=False):
        top, bottom, glow = (self.dtop, self.dbottom, self.dglow) if dark else (self.top, self.bottom, self.glow)
        img = gradient((S, S), top, bottom).rotate(45, resample=Image.BICUBIC, expand=True)
        img = img.resize((S * 2, S * 2)).crop((S // 2, S // 2, S * 3 // 2, S * 3 // 2))
        if glow:
            img.alpha_composite(radial(70, 50, 190, (255, 255, 255, glow), (255, 255, 255, 0)))
        return img

    def flatten(self, dark=False):
        out = blank()
        out.alpha_composite(Image.composite(self.background(dark), blank(), leaf_mask(self.box)))
        out.alpha_composite(self.fg)
        return out


class Art:
    def __init__(self, top, bottom, box=BODY, glow=70):
        self.box = box
        self.mask = leaf_mask(box)
        self.top, self.bottom, self.glow = top, bottom, glow
        self.img = blank()

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
            self.img.alpha_composite(colored(ImageChops.multiply(hi, m), (255, 255, 255, light * 2 // 3)))
            self.img.alpha_composite(colored(ImageChops.multiply(lo, m), (0, 0, 0, shade * 2 // 3)))
            self.img.alpha_composite(colored(specular(m, 2), (255, 255, 255, min(255, light + 90))))
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
        """The finished tile: a soft sheen over the top, the bright glass rim, a faint inner glow, the shadow below."""
        top = blank()
        top.alpha_composite(self.img)
        x0, y0, x1, y1 = self.box
        top.alpha_composite(radial(x0 + (x1 - x0) * 0.22, y0 + (y1 - y0) * 0.12, (x1 - x0) * 0.75, (255, 255, 255, 40),
                                   (255, 255, 255, 0)))
        inner = ImageChops.subtract(self.mask, self.mask.filter(ImageFilter.MinFilter(17))).filter(ImageFilter.GaussianBlur(4))
        top.alpha_composite(colored(inner, (255, 255, 255, 24)))
        top.alpha_composite(colored(bands(self.mask, 4)[1], (0, 0, 0, 34)))
        top.alpha_composite(colored(specular(self.mask, 3), (255, 255, 255, 215)))
        fg = blank()
        if shadow:
            sh = colored(self.mask, (0, 0, 0, shadow * 3 // 4)).filter(ImageFilter.GaussianBlur(5))
            out = blank()
            out.alpha_composite(sh, (0, 4))
            out.putalpha(ImageChops.multiply(out.getchannel("A"), ImageChops.invert(self.mask)))
            fg.alpha_composite(out)
        fg.alpha_composite(Image.composite(top, blank(), self.mask))
        return Tile(fg, self.top, self.bottom, self.glow, self.box)


def gloss(box):
    """The soft reflection on the upper half of a round or glassy object."""
    x0, y0, x1, y1 = box
    return ellipse4((x0 + (x1 - x0) * 0.12, y0 + (y1 - y0) * 0.04, x1 - (x1 - x0) * 0.12, y0 + (y1 - y0) * 0.5))


# ---------------------------------------------------------------- the SynthOS icon language
#
# App tiles are "leaves": three wide round corners and one tight one at the bottom right, filled with a diagonal
# blend and a soft light from the top left. On them sits one bold, flat glyph in white (a little shaded, with a soft
# shadow) and at most one accent color. Folders are indigo sleeves with the tab on the right; documents are pages with
# a colored spine and their corner turned at the bottom.

WHITE = ((255, 255, 255, 255), (228, 233, 244, 255))


def glyph(art, shape, paint=WHITE, shadow=55):
    art.put(shape, paint, shadow)
    return art


def ring4(cx, cy, r, w):
    return ellipse4((cx - r, cy - r, cx + r, cy + r), True, w)


def disc4(cx, cy, r):
    return ellipse4((cx - r, cy - r, cx + r, cy + r))


def arc4(box, start, end, width):
    return lambda d: d.arc(box, start, end, fill=255, width=width)


def minus(m1, m2):
    return ImageChops.subtract(mask_of(m1) if not isinstance(m1, Image.Image) else m1,
                               mask_of(m2) if not isinstance(m2, Image.Image) else m2)


def union(*ms):
    out = Image.new("L", (S, S), 0)
    for m in ms:
        out = ImageChops.lighter(out, m if isinstance(m, Image.Image) else mask_of(m))
    return out


# -------- folders: an indigo sleeve, the tab on the right, the emblem pressed into the front

FOLDER_BACK = (hexc("5b5fe0"), hexc("3a36a8"))
FOLDER_FRONT = (hexc("9aa2ff"), hexc("6466f1"))


def folder_base(emblem=None):
    img = blank()
    back = union(rr4((28, 58, 228, 214), 22), rr4((132, 40, 224, 84), 16))
    img.alpha_composite(colored(back, (0, 0, 0, 70)).filter(ImageFilter.GaussianBlur(7)), (0, 6))
    img.alpha_composite(paint_of(FOLDER_BACK, back))
    sheet = mask_of(rr4((48, 70, 208, 150), 10))
    img.alpha_composite(paint_of((hexc("ffffff"), hexc("e6e8f5")), sheet))
    front = mask_of(lambda d: d.polygon([(28, 104), (228, 92), (228, 196), (206, 218), (50, 218), (28, 196)],
                                        fill=255))
    front = ImageChops.multiply(front, mask_of(rr4((28, 88, 228, 218), 22)))
    img.alpha_composite(colored(front, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(5)), (0, -2))
    img.alpha_composite(paint_of(FOLDER_FRONT, front))
    img.alpha_composite(colored(specular(front, 2), (255, 255, 255, 170)))
    if emblem:
        m = mask_of(emblem)
        img.alpha_composite(colored(m, (40, 36, 140, 150)), (0, 2))
        img.alpha_composite(colored(m, (255, 255, 255, 200)))
    return img


def folder_icon():
    return folder_base()


def home_folder_icon():
    return folder_base(lambda d: (d.polygon([(128, 116), (166, 148), (156, 148), (156, 186), (100, 186), (100, 148),
                                             (90, 148)], fill=255),
                                  d.rectangle([120, 162, 136, 186], fill=0)))


def apps_folder_icon():
    def draw(d):
        for i in range(4):
            x, y = 100 + (i % 2) * 32, 122 + (i // 2) * 32
            d.rounded_rectangle([x, y, x + 24, y + 24], radius=8, fill=255)
    return folder_base(draw)


def docs_folder_icon():
    def draw(d):
        d.rounded_rectangle([102, 118, 154, 188], radius=8, fill=255)
        for y in (134, 150, 166):
            d.rectangle([112, y, 144, y + 5], fill=0)
    return folder_base(draw)


def desktop_folder_icon():
    def draw(d):
        d.rounded_rectangle([92, 120, 164, 170], radius=8, fill=255)
        d.rounded_rectangle([100, 128, 156, 162], radius=4, fill=0)
        d.rounded_rectangle([112, 176, 144, 184], radius=3, fill=255)
    return folder_base(draw)


def downloads_folder_icon():
    def draw(d):
        d.rectangle([120, 114, 136, 150], fill=255)
        d.polygon([(100, 146), (156, 146), (128, 176)], fill=255)
        d.rounded_rectangle([96, 180, 160, 188], radius=4, fill=255)
    return folder_base(draw)


# -------- documents: a page with a colored spine and the corner turned at the bottom right

def doc_base(spine=("8a93a6", "5b6478"), emblem=None, label=None):
    def page(d):
        d.rounded_rectangle([56, 22, 204, 234], radius=18, fill=255)
        d.polygon([(204, 184), (204, 234), (154, 234)], fill=0)
    m = mask_of(page)
    img = blank()
    img.alpha_composite(colored(m, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(6)), (0, 5))
    img.alpha_composite(paint_of((hexc("ffffff"), hexc("eef0f5")), m))
    img.alpha_composite(colored(ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(3))), (0, 0, 0, 38)))
    spine_m = ImageChops.multiply(m, mask_of(rr4((56, 22, 84, 234), 0)))
    img.alpha_composite(paint_of((hexc(spine[0]), hexc(spine[1])), spine_m))
    turn = mask_of(lambda d: d.polygon([(204, 184), (154, 234), (160, 190)], fill=255))
    img.alpha_composite(colored(turn, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(4)), (-3, -3))
    img.alpha_composite(paint_of((hexc("f6f7fa"), hexc("cdd3dd")), turn))
    if emblem:
        emblem(img)
    if label:
        f = font(INTER_BOLD, 34 * 4)
        big = Image.new("L", (S * 4, S * 4), 0)
        ImageDraw.Draw(big).text((140 * 4, 128 * 4), label, font=f, fill=255, anchor="mm")
        lm = big.resize((S, S), Image.LANCZOS)
        img.alpha_composite(colored(lm, hexc(spine[1])))
    return img


def blank_doc():
    return doc_base()


def text_doc():
    def lines(img):
        for i, w in enumerate((96, 80, 96, 64, 90, 52)):
            y = 54 + i * 22
            img.alpha_composite(colored(mask_of(rr4((100, y, 100 + w, y + 8), 4)), (150, 158, 176, 255)))
    return doc_base(("6a9bff", "3a62d8"), lines)


def image_doc():
    def pic(img):
        frame = mask_of(rr4((98, 70, 190, 158), 12))
        img.alpha_composite(paint_of((hexc("8fd8ff"), hexc("d8f1ff")), frame))
        img.alpha_composite(paint_of((hexc("ffd65a"), hexc("ff9f2e")), ImageChops.multiply(mask_of(disc4(166, 94, 11)),
                                                                                          frame)))
        img.alpha_composite(paint_of((hexc("42c98a"), hexc("1b8a5a")),
                                     ImageChops.multiply(mask_of(poly4([(98, 158), (130, 112), (154, 140), (168, 126),
                                                                        (190, 158)])), frame)))
    return doc_base(("ff9f43", "e8590c"), pic)


def dll_doc():
    def nut(img):
        pts = [(144 + 40 * math.cos(math.pi / 3 * i + math.pi / 6), 120 + 40 * math.sin(math.pi / 3 * i + math.pi / 6))
               for i in range(6)]
        m = minus(poly4(pts), disc4(144, 120, 16))
        img.alpha_composite(paint_of((hexc("a0a8ba"), hexc("5d667a")), m))
    return doc_base(("9aa4b5", "4b5568"), nut)


def dos_doc():
    return doc_base(("3a3f4b", "151821"), label="C:\\")


def sx_doc():
    return doc_base(("a46bff", "5b2bd1"), label="SX")


def script_doc():
    return doc_base(("2fd27a", "11894a"), label=">_")


# -------- other free-standing icons

def info_icon():
    art = Art(hexc("4facfe"), hexc("2160d8"))
    glyph(art, ring4(128, 128, 72, 14))
    glyph(art, disc4(128, 92, 11))
    glyph(art, rr4((118, 112, 138, 170), 8))
    return art.done()


def power_icon():
    art = Art(hexc("ff6b7a"), hexc("c81d4f"))
    glyph(art, minus(arc4((62, 66, 194, 198), -55, 235, 18), rr4((114, 40, 142, 120), 0)))
    glyph(art, rr4((119, 52, 137, 132), 9))
    return art.done()


def warning_icon():
    img = blank()
    tri = mask_of(lambda d: d.polygon([(128, 30), (234, 214), (22, 214)], fill=255)).filter(ImageFilter.GaussianBlur(6))
    tri = tri.point(lambda v: 255 if v > 128 else v * 2)
    img.alpha_composite(colored(tri, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(6)), (0, 6))
    img.alpha_composite(paint_of((hexc("ffcf4a"), hexc("ff8a1f")), tri))
    img.alpha_composite(colored(mask_of(rr4((118, 92, 138, 168), 10)), (255, 255, 255, 255)))
    img.alpha_composite(colored(mask_of(disc4(128, 188, 12)), (255, 255, 255, 255)))
    return img


def user_icon():
    img = blank()
    c = mask_of(disc4(128, 128, 100))
    img.alpha_composite(colored(c, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(6)), (0, 5))
    img.alpha_composite(paint_of((hexc("a78bfa"), hexc("5b3fd1")), c))
    bust = ImageChops.multiply(union(disc4(128, 100, 38), ellipse4((58, 148, 198, 268))), c)
    img.alpha_composite(paint_of(WHITE, bust))
    return img


def trash_icon(full=False):
    img = blank()
    body = mask_of(lambda d: d.polygon([(62, 78), (194, 78), (180, 222), (76, 222)], fill=255))
    body = ImageChops.multiply(body, mask_of(rr4((40, 60, 216, 230), 22)))
    img.alpha_composite(colored(body, (0, 0, 0, 60)).filter(ImageFilter.GaussianBlur(6)), (0, 6))
    img.alpha_composite(paint_of((hexc("8d97ab"), hexc("4a5367")), body))
    for x in (102, 128, 154):
        img.alpha_composite(colored(mask_of(rr4((x - 6, 104, x + 6, 196), 6)), (30, 36, 50, 110)))
    if full:
        for cx, cy, r, t in ((100, 70, 26, "ffffff"), (140, 62, 30, "f1f3f8"), (172, 76, 22, "e4e8f0")):
            m = mask_of(disc4(cx, cy, r))
            img.alpha_composite(colored(m, (0, 0, 0, 40)).filter(ImageFilter.GaussianBlur(3)), (0, 3))
            img.alpha_composite(paint_of((hexc(t), hexc("cfd5df")), m))
    lid = mask_of(rr4((48, 52, 208, 76), 12))
    if full:
        lid = lid.rotate(-12, resample=Image.BICUBIC, center=(200, 64), translate=(0, -14))
    img.alpha_composite(colored(lid, (0, 0, 0, 50)).filter(ImageFilter.GaussianBlur(3)), (0, 3))
    img.alpha_composite(paint_of((hexc("b4bdcd"), hexc("6d778b")), lid))
    img.alpha_composite(paint_of((hexc("b4bdcd"), hexc("6d778b")),
                                 mask_of(rr4((108, 40, 148, 58), 8)).rotate(-12 if full else 0, resample=Image.BICUBIC,
                                                                            center=(200, 64),
                                                                            translate=(0, -14 if full else 0))))
    return img


def trash_full_icon():
    return trash_icon(True)


def computer_art():
    img = blank()
    top = mask_of(lambda d: d.polygon([(40, 112), (128, 70), (216, 112), (128, 154)], fill=255))
    side_l = mask_of(lambda d: d.polygon([(40, 112), (128, 154), (128, 194), (40, 152)], fill=255))
    side_r = mask_of(lambda d: d.polygon([(128, 154), (216, 112), (216, 152), (128, 194)], fill=255))
    whole = union(top, side_l, side_r)
    img.alpha_composite(colored(whole, (0, 0, 0, 70)).filter(ImageFilter.GaussianBlur(8)), (0, 10))
    img.alpha_composite(paint_of((hexc("e9edf5"), hexc("b8c0d0")), top))
    img.alpha_composite(paint_of((hexc("8c95a8"), hexc("5d667a")), side_l))
    img.alpha_composite(paint_of((hexc("6c7589"), hexc("3f475a")), side_r))
    img.alpha_composite(colored(mask_of(line4([(150, 166), (198, 143)], 6)), (64, 224, 196, 255)))
    img.alpha_composite(colored(mask_of(lambda d: d.polygon([(96, 106), (128, 91), (160, 106), (128, 121)], fill=255)),
                                (120, 104, 255, 160)))
    return img


# -------- app tiles

def files_art():
    """Three cards fanned out, the front one with a tab: where files live."""
    art = Art(hexc("49d6c8"), hexc("1667a8"))
    for ang, c in ((16, (255, 255, 255, 120)), (6, (255, 255, 255, 170))):
        art.put(rotated(rr4((62, 72, 194, 182), 18), ang, 128, 200), c, 30)
    front = union(rr4((52, 90, 204, 206), 20), rr4((60, 74, 128, 104), 12))
    glyph(art, front)
    art.put(rr4((76, 132, 180, 142), 5), hexc("2a8fb8"))
    art.put(rr4((76, 156, 150, 166), 5), hexc("8fd0e0"))
    return art.done()


def terminal_art():
    art = Art(hexc("2c3242"), hexc("0b0e15"))
    art.put(line4([(68, 86), (112, 124), (68, 162)], 20), (hexc("4dffb0"), hexc("16c784")), 40)
    glyph(art, rr4((126, 154, 190, 172), 9))
    art.put(ellipse4((40, 34, 216, 120)), ((80, 255, 180, 26), (80, 255, 180, 0)), blur=14)
    return art.done()


def settings_art():
    """Three sliders, each knob in its own color."""
    art = Art(hexc("8b95ab"), hexc("343c50"))
    for y, x, c in ((82, 158, "ff9f43"), (128, 96, "4dd6c3"), (174, 140, "a78bfa")):
        glyph(art, rr4((50, y - 7, 206, y + 7), 7), ((255, 255, 255, 200), (230, 235, 245, 200)), 20)
        art.put(disc4(x, y, 19), (hexc("ffffff"), hexc("e8ecf4")), 60)
        art.put(disc4(x, y, 10), (lighter(hexc(c), 0.2), hexc(c)))
    return art.done()


def calc_art():
    art = Art(hexc("ffa94d"), hexc("e8590c"))
    w = 16
    glyph(art, union(rr4((60, 88 - w // 2, 116, 88 + w // 2), 8), rr4((88 - w // 2, 60, 88 + w // 2, 116), 8)))
    glyph(art, rr4((140, 88 - w // 2, 196, 88 + w // 2), 8))
    glyph(art, union(rotated(rr4((60, 168 - w // 2, 116, 168 + w // 2), 8), 45, 88, 168),
                     rotated(rr4((60, 168 - w // 2, 116, 168 + w // 2), 8), -45, 88, 168)))
    glyph(art, union(rr4((140, 150, 196, 164), 7), rr4((140, 174, 196, 188), 7)))
    return art.done()


def notepad_art():
    art = Art(hexc("ffe17a"), hexc("f29f1b"))
    sheet = rotated(rr4((60, 50, 184, 206), 18), -6)
    glyph(art, sheet)
    for i in range(4):
        art.clip(rotated(rr4((80, 86 + i * 26, 160 - (30 if i == 3 else 0), 94 + i * 26), 4), -6), hexc("f2b33d"), sheet)
    nib = rotated(poly4([(150, 196), (166, 156), (220, 102), (234, 116), (180, 170)]), 0)
    art.put(nib, (hexc("4a3a20"), hexc("1f170a")), 50)
    art.put(poly4([(150, 196), (158, 176), (170, 188)]), hexc("ffe17a"))
    return art.done()


def paint_art():
    """A broad brush stroke sweeping across, a drop falling from its end."""
    art = Art(hexc("ff7eb6"), hexc("7b3dff"))
    glyph(art, line4([(56, 170), (90, 120), (130, 150), (170, 100), (200, 76)], 30))
    art.put(lambda d: d.polygon([(190, 132), (204, 160), (190, 176), (176, 160)], fill=255), (hexc("ffe066"),
                                                                                            hexc("ffb020")), 40)
    art.put(disc4(190, 164, 14), (hexc("ffe066"), hexc("ffb020")))
    return art.done()


def monitor_art():
    """Activity: bars rising, a pulse across them."""
    art = Art(hexc("24d7a8"), hexc("0a6170"))
    for i, h in enumerate((50, 86, 64, 112, 80)):
        x = 54 + i * 32
        glyph(art, rr4((x, 196 - h, x + 20, 196), 8), ((255, 255, 255, 230), (220, 245, 240, 200)), 30)
    art.put(line4([(44, 128), (86, 128), (104, 92), (128, 166), (150, 112), (168, 128), (212, 128)], 9),
            (hexc("fff27a"), hexc("ffb020")), 40)
    return art.done()


def hexagon(cx, cy, r):
    return poly4([(cx + r * math.cos(math.pi / 3 * i + math.pi / 6), cy + r * math.sin(math.pi / 3 * i + math.pi / 6))
                  for i in range(6)])


def mines_art():
    art = Art(hexc("8190ff"), hexc("3a3cc0"))
    for cx, cy in ((92, 100), (164, 100), (128, 162), (56, 162), (200, 162)):
        glyph(art, hexagon(cx, cy, 36), ((255, 255, 255, 235), (225, 230, 255, 220)), 30)
    art.put(rr4((124, 120, 132, 196), 4), hexc("2a2c6a"))
    art.put(poly4([(132, 122), (168, 136), (132, 152)]), (hexc("ff6b6b"), hexc("e03131")))
    art.put(disc4(92, 100, 14), hexc("2a2c6a"))
    return art.done()


def logo_art():
    art = Art(hexc("ff9a62"), hexc("6b3a8f"))
    m = mirage_mask((S, S), 128, 124, 66)
    glyph(art, m, (hexc("fffaf0"), hexc("ffd59a")), 60)
    return art.done()


def launchpad_art():
    """Four orbs around a spark: everything in one place."""
    art = Art(hexc("6c5cff"), hexc("22197a"))
    for (cx, cy), c in zip(((92, 92), (164, 92), (92, 164), (164, 164)), ("ff8fa3", "ffd166", "4dd6c3", "8fb4ff")):
        art.put(disc4(cx, cy, 30), (lighter(hexc(c), 0.35), hexc(c)), 50)
    glyph(art, poly4([(128, 104), (136, 120), (152, 128), (136, 136), (128, 152), (120, 136), (104, 128), (120, 120)]))
    return art.done()


def exe_art():
    """A cube, its three faces lit differently: a program."""
    art = Art(hexc("a0aabc"), hexc("454e62"))
    top = poly4([(128, 52), (196, 90), (128, 128), (60, 90)])
    left = poly4([(60, 98), (124, 134), (124, 208), (60, 170)])
    right = poly4([(132, 134), (196, 98), (196, 170), (132, 208)])
    art.put(top, WHITE, 40)
    art.put(left, ((230, 235, 245, 255), (200, 208, 224, 255)), 30)
    art.put(right, (hexc("7c8cff"), hexc("4d55d6")), 30)
    return art.done()


def usb_art():
    art = Art(hexc("ffb35c"), hexc("e8590c"))
    glyph(art, union(rr4((121, 52, 135, 190), 7), line4([(128, 150), (88, 120), (88, 98)], 12),
                     line4([(128, 132), (168, 106), (168, 92)], 12)))
    glyph(art, disc4(128, 196, 18))
    glyph(art, poly4([(128, 34), (146, 62), (110, 62)]))
    glyph(art, disc4(88, 92, 12))
    glyph(art, rr4((156, 76, 180, 98), 4))
    return art.done()


def browser_art():
    """Obscura: a lens with its aperture blades, a beam of light passing through."""
    art = Art(hexc("27c6ff"), hexc("6a3dff"))
    art.put(poly4([(20, 40), (128, 112), (128, 144), (20, 110)]), ((255, 255, 255, 70), (255, 255, 255, 0)), blur=3)
    glyph(art, ring4(128, 128, 70, 18))
    blades = Image.new("L", (S, S), 0)
    for i in range(6):
        a0 = math.pi / 3 * i
        p = [(128 + 52 * math.cos(a0), 128 + 52 * math.sin(a0)),
             (128 + 52 * math.cos(a0 + math.pi / 3), 128 + 52 * math.sin(a0 + math.pi / 3)),
             (128 + 22 * math.cos(a0 + math.pi / 2.2), 128 + 22 * math.sin(a0 + math.pi / 2.2))]
        blades = ImageChops.lighter(blades, mask_of(poly4(p)))
    art.put(blades, ((255, 255, 255, 190), (220, 228, 255, 170)), 20)
    art.put(disc4(128, 128, 18), (hexc("ffe28a"), hexc("ffb020")))
    return art.done()


def photos_art():
    art = Art(hexc("ffd34f"), hexc("ff6a3d"))
    frame = minus(rr4((48, 58, 208, 198), 26), rr4((66, 76, 190, 180), 14))
    glyph(art, frame)
    glyph(art, poly4([(66, 180), (106, 122), (136, 156), (156, 134), (190, 180)]))
    glyph(art, disc4(160, 104, 15))
    return art.done()


def music_art():
    art = Art(hexc("ff6b9a"), hexc("b5175e"))
    glyph(art, union(rr4((94, 66, 110, 176), 6), rr4((164, 54, 180, 164), 6),
                     poly4([(94, 66), (180, 46), (180, 72), (94, 92)])))
    glyph(art, ellipse4((58, 152, 112, 196)))
    glyph(art, ellipse4((128, 140, 182, 184)))
    return art.done()


def video_art():
    art = Art(hexc("9b7bff"), hexc("4422c9"))
    film = minus(rr4((44, 68, 212, 188), 22), poly4([(112, 98), (112, 158), (162, 128)]))
    glyph(art, film)
    for x in range(64, 200, 26):
        art.put(rr4((x, 76, x + 12, 86), 3), hexc("6a4ae0"))
        art.put(rr4((x, 170, x + 12, 180), 3), hexc("6a4ae0"))
    return art.done()


def viewer_art():
    """A viewfinder with an eye in it: looking at a picture."""
    art = Art(hexc("3fd0e6"), hexc("1d5fc4"))
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        cx, cy = 128 - sx * 74, 128 - sy * 64
        glyph(art, line4([(cx, cy + sy * 30), (cx, cy), (cx + sx * 30, cy)], 14))
    glyph(art, ellipse4((62, 90, 194, 166)))
    art.put(disc4(128, 128, 24), (hexc("1d5fc4"), hexc("12408f")))
    art.put(disc4(136, 120, 7), (255, 255, 255, 230))
    return art.done()


def installer_art():
    """An open box with an arrow going in."""
    art = Art(hexc("5ee38c"), hexc("158a52"))
    glyph(art, poly4([(52, 120), (128, 150), (204, 120), (204, 186), (128, 216), (52, 186)]))
    art.put(poly4([(52, 120), (128, 150), (128, 216), (52, 186)]), ((215, 235, 225, 255), (190, 215, 205, 255)))
    art.put(rr4((116, 36, 140, 104), 8), (hexc("fff27a"), hexc("ffc21a")), 40)
    art.put(poly4([(92, 96), (164, 96), (128, 136)]), (hexc("ffd84a"), hexc("ffb020")), 40)
    return art.done()


def screenshot_art():
    art = Art(hexc("b9c1cf"), hexc("4f5a72"))
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        cx, cy = 128 - sx * 74, 128 - sy * 70
        glyph(art, line4([(cx, cy + sy * 34), (cx, cy), (cx + sx * 34, cy)], 14))
    glyph(art, ring4(128, 128, 30, 12))
    art.put(disc4(128, 128, 10), (hexc("ff6b6b"), hexc("e03131")))
    return art.done()


def store_art():
    """A cut gem."""
    art = Art(hexc("45e3d3"), hexc("1462c0"))
    outline = poly4([(70, 84), (186, 84), (214, 118), (128, 214), (42, 118)])
    glyph(art, outline)
    art.put(poly4([(42, 118), (214, 118), (128, 214)]), ((214, 236, 255, 255), (180, 210, 245, 255)))
    art.put(poly4([(98, 118), (158, 118), (128, 214)]), ((240, 248, 255, 255), (200, 225, 250, 255)))
    art.put(poly4([(70, 84), (98, 118), (128, 84)]), ((235, 245, 255, 255), (215, 230, 250, 255)))
    art.put(poly4([(128, 84), (158, 118), (186, 84)]), ((235, 245, 255, 255), (215, 230, 250, 255)))
    return art.done()


def code_art():
    art = Art(hexc("31375a"), hexc("0e1020"))
    glyph(art, line4([(98, 64), (72, 70), (72, 118), (52, 128), (72, 138), (72, 186), (98, 192)], 14))
    glyph(art, line4([(158, 64), (184, 70), (184, 118), (204, 128), (184, 138), (184, 186), (158, 192)], 14))
    art.put(line4([(140, 86), (116, 170)], 14), (hexc("5ee7ff"), hexc("3a8bff")), 40)
    return art.done()


def games_art():
    art = Art(hexc("ff7a59"), hexc("c92a55"))
    pad = union(rr4((44, 92, 212, 176), 42), ellipse4((40, 120, 112, 210)), ellipse4((144, 120, 216, 210)))
    pad = minus(pad, union(rr4((76, 112, 88, 152), 4), rr4((62, 126, 102, 138), 4)))
    pad = minus(pad, union(disc4(164, 118, 9), disc4(186, 140, 9)))
    glyph(art, pad)
    return art.done()


def clock_art():
    """A ring that fills like a timer, two hands in it."""
    art = Art(hexc("2c3550"), hexc("0c111f"))
    art.put(ring4(128, 128, 74, 14), (255, 255, 255, 50))
    art.put(arc4((54, 54, 202, 202), -90, 160, 14), (hexc("ffb347"), hexc("ff5f6d")), 30)
    glyph(art, rr4((122, 74, 134, 134), 6))
    glyph(art, rotated(rr4((122, 122, 134, 170), 6), -60, 128, 128))
    art.put(disc4(128, 128, 9), hexc("ff7a59"))
    return art.done()


def weather_art():
    art = Art(hexc("5bc8ff"), hexc("2a6fdf"))
    art.put(disc4(98, 98, 40), (hexc("ffe680"), hexc("ffb020")), 40)
    glyph(art, union(disc4(116, 150, 34), disc4(156, 136, 42), disc4(190, 162, 26), rr4((84, 150, 214, 188), 19)))
    return art.done()


def notes_art():
    art = Art(hexc("ffe36b"), hexc("f4a700"))
    ink = (hexc("5a4300"), hexc("3a2b00"))
    for i in range(3):
        y = 80 + i * 46
        art.put(line4([(58, y), (70, y + 12), (92, y - 12)], 10), ink, 0)
        glyph(art, rr4((110, y - 7, 200 - (30 if i == 2 else 0), y + 7), 7))
    return art.done()


def console_art():
    art = Art(hexc("3c4252"), hexc("12151d"))
    for i, (c, w) in enumerate((("4dffb0", 100), ("4dffb0", 80), ("ffd166", 110), ("ff6b6b", 70), ("4dffb0", 90))):
        y = 66 + i * 28
        art.put(disc4(64, y + 5, 8), hexc(c))
        glyph(art, rr4((86, y, 86 + w, y + 10), 5), ((255, 255, 255, 220), (225, 230, 240, 200)), 20)
    return art.done()


def sysinfo_art():
    art = Art(hexc("5584ff"), hexc("1a3aa8"))
    for i in range(4):
        o = 76 + i * 30
        for p in (rr4((o, 40, o + 12, 64), 4), rr4((o, 192, o + 12, 216), 4), rr4((40, o, 64, o + 12), 4),
                  rr4((192, o, 216, o + 12), 4)):
            art.put(p, (255, 255, 255, 200))
    glyph(art, minus(rr4((62, 62, 194, 194), 24), rr4((96, 96, 160, 160), 12)))
    art.put(rr4((106, 106, 150, 150), 8), (hexc("ffd166"), hexc("ff9f43")))
    return art.done()


def netutil_art():
    art = Art(hexc("27d8a8"), hexc("0a6f86"))
    pts = ((128, 70), (70, 170), (186, 170))
    glyph(art, union(line4([pts[0], pts[1]], 10), line4([pts[1], pts[2]], 10), line4([pts[2], pts[0]], 10)),
          ((255, 255, 255, 200), (230, 245, 240, 200)), 20)
    for i, (x, y) in enumerate(pts):
        glyph(art, disc4(x, y, 24))
        art.put(disc4(x, y, 10), hexc(("ffd166", "ff7a59", "8fb4ff")[i]))
    return art.done()


def diskutil_art():
    art = Art(hexc("c8cfdc"), hexc("5f6a80"))
    glyph(art, disc4(128, 128, 78))
    art.put(lambda d: d.pieslice([50, 50, 206, 206], -90, 40, fill=255), (hexc("4dd6c3"), hexc("1a8fb0")))
    art.put(disc4(128, 128, 22), (hexc("5f6a80"), hexc("3c4558")))
    art.put(disc4(128, 128, 8), (255, 255, 255, 255))
    return art.done()


def timeline_art():
    art = Art(hexc("a876ff"), hexc("4a1fb8"))
    glyph(art, arc4((52, 52, 204, 204), 200, 520, 16))
    glyph(art, poly4([(40, 104), (78, 128), (78, 86)]))
    for i, y in enumerate((96, 128, 160)):
        art.put(disc4(128, y, 11), (255, 255, 255, 255) if i == 1 else (255, 255, 255, 150))
    return art.done()


def reader_art():
    """An open book."""
    art = Art(hexc("ff8a5c"), hexc("d1352f"))
    glyph(art, lambda d: d.polygon([(124, 78), (124, 196), (90, 182), (46, 186), (46, 70), (90, 64)], fill=255))
    glyph(art, lambda d: d.polygon([(132, 78), (132, 196), (166, 182), (210, 186), (210, 70), (166, 64)], fill=255))
    for i in range(3):
        y = 96 + i * 24
        art.put(rr4((62, y, 108, y + 6), 3), hexc("f2a58a"))
        art.put(rr4((148, y, 194, y + 6), 3), hexc("f2a58a"))
    return art.done()


def assistant_art():
    art = Art(hexc("3b2a8f"), hexc("100a36"))
    art.put(disc4(128, 128, 76), ((120, 160, 255, 90), (180, 90, 255, 30)), 0, 0, blur=10)
    art.put(disc4(128, 128, 52), (hexc("8fb4ff"), hexc("7a4dff")), 40, 4)

    def spark(cx, cy, r, w):
        pts = []
        for i in range(8):
            a = -math.pi / 2 + i * math.pi / 4
            rr = r if i % 2 == 0 else w
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        return poly4(pts)
    glyph(art, spark(128, 128, 62, 14), WHITE, 30)
    glyph(art, spark(184, 74, 22, 6), WHITE, 0)
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
    ("ICON_GAMES", games_art), ("ICON_CLOCK", clock_art), ("ICON_WEATHER", weather_art), ("ICON_NOTES", notes_art),
    ("ICON_CONSOLE", console_art), ("ICON_SYSINFO", sysinfo_art), ("ICON_NETUTIL", netutil_art),
    ("ICON_DISKUTIL", diskutil_art), ("ICON_TIMELINE", timeline_art), ("ICON_READER", reader_art),
    ("ICON_ASSISTANT", assistant_art),
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



def sym_sparkle(d):
    """the assistant: a large four-pointed spark and a small one"""
    def spark(cx, cy, r, w):
        pts = []
        for i in range(8):
            a = -math.pi / 2 + i * math.pi / 4
            rr = r if i % 2 == 0 else w
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        d.polygon(pts, fill=255)
    spark(56, 72, 52, 13)
    spark(102, 26, 22, 6)


def sym_mic(d):
    d.rounded_rectangle([42, 8, 86, 82], radius=22, fill=255)
    d.arc([22, 34, 106, 104], 0, 180, fill=255, width=W)
    d.line([(64, 104), (64, 122)], fill=255, width=W)
    d.line([(42, 122), (86, 122)], fill=255, width=W)

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
    ("SYM_COPY", sym_copy), ("SYM_SPARKLE", sym_sparkle), ("SYM_MIC", sym_mic),
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
    tiles = {}
    for name, fn in ICONS:
        art = fn()
        img = art.fg if isinstance(art, Tile) else art
        if isinstance(art, Tile):
            tiles[name] = art
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
        packed = lz4pack.compress(lz4pack.planes(argb(img), SIZE))
        entries.append((name, len(pack), len(packed)))
        pack += packed
    pak = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rootfs", "system", "icons.pak")
    with open(pak, "wb") as f:
        f.write(pack)
    print(f"const uint32_t icon_pack_size = {len(pack)};\n")
    def rgb(c):
        return f"0x{c[0]:02x}{c[1]:02x}{c[2]:02x}"
    print("const struct icon_entry icon_pack[ICON_COUNT] = {")
    for name, offset, n in entries:
        t = tiles.get(name)
        tile = (f", {{{rgb(t.top)}, {rgb(t.bottom)}, {t.glow}}}, {{{rgb(t.dtop)}, {rgb(t.dbottom)}, {t.dglow}}}"
                if t else "")
        print(f"    [{name}] = {{{SIZE}, {n}, {offset}{tile}}},")
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
