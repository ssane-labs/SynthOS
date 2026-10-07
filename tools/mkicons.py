#!/usr/bin/env python3
"""Draws the SynthOS desktop icons and symbols and writes them as compressed C source.

Icons are 64x64 ARGB (squircle app icons, folders, documents); symbols are monochrome
alpha masks at 16 and 32 px that the desktop tints at run time.
usage: tools/mkicons.py > kernel/gui/icons.c (needs Pillow)
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lz4pack  # noqa: E402

S = 256
SIZE = 64
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


def gradient(size, top, bottom, horizontal=False):
    w, h = size
    img = Image.new("RGBA", size)
    d = ImageDraw.Draw(img)
    span = w if horizontal else h
    for i in range(span):
        t = i / max(1, span - 1)
        c = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(4))
        if horizontal:
            d.line([(i, 0), (i, h)], fill=c)
        else:
            d.line([(0, i), (w, i)], fill=c)
    return img


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


def drop_shadow(img, mask, offset=6, blur=8, alpha=90):
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    m = mask.point(lambda v: v * alpha // 255)
    shadow.putalpha(m)
    shadow = shadow.transform((S, S), Image.AFFINE, (1, 0, 0, 0, 1, -offset))
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur))
    out = blank()
    out.alpha_composite(shadow)
    out.alpha_composite(img)
    return out


BODY = (26, 26, 230, 230)


def app_base(top, bottom, glyph=None):
    """Squircle app icon body with gradient, top sheen, inner rim and soft shadow."""
    mask = squircle_mask(BODY)
    layer = blank()
    layer.paste(gradient((S, S), top, bottom), (0, 0), mask)
    if glyph:
        g = blank()
        glyph(g, ImageDraw.Draw(g))
        layer.alpha_composite(Image.composite(g, blank(), mask))
    band = Image.new("L", (S, S), 0)
    ImageDraw.Draw(band).rectangle([0, 0, S, 118], fill=255)
    sheen = gradient((S, S), (255, 255, 255, 56), (255, 255, 255, 0))
    layer.alpha_composite(Image.composite(sheen, blank(), ImageChops.multiply(mask, band)))
    inner = squircle_mask((BODY[0] + 3, BODY[1] + 3, BODY[2] - 3, BODY[3] - 3))
    edge = ImageChops.subtract(mask, inner).point(lambda v: v * 80 // 255)
    rim = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    rim.putalpha(edge)
    layer.alpha_composite(rim)
    return drop_shadow(layer, mask, 6, 7, 80)


def page_base(emblem=None):
    mask = rounded_mask((52, 20, 204, 236), 10)
    img = blank()
    body = gradient((S, S), (255, 255, 255, 255), (238, 238, 242, 255))
    img.paste(body, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.polygon([(160, 20), (204, 64), (160, 64)], fill=(214, 216, 222, 255))
    d.polygon([(160, 20), (204, 20), (204, 64)], fill=(0, 0, 0, 0))
    outline = blank()
    od = ImageDraw.Draw(outline)
    od.rounded_rectangle([52, 20, 204, 236], radius=10, outline=(0, 0, 0, 40), width=3)
    od.polygon([(160, 20), (204, 20), (204, 64)], fill=(0, 0, 0, 0))
    img.alpha_composite(outline)
    d.line([(160, 20), (160, 64), (204, 64)], fill=(0, 0, 0, 40), width=3)
    clear = Image.new("L", (S, S), 255)
    ImageDraw.Draw(clear).polygon([(162, 18), (206, 18), (206, 62)], fill=0)
    img.putalpha(Image.composite(img.getchannel("A"), Image.new("L", (S, S), 0), clear))
    if emblem:
        emblem(img, ImageDraw.Draw(img))
    return drop_shadow(img, img.getchannel("A"), 4, 5, 70)


FOLDER_TOP = (126, 196, 252, 255)
FOLDER_BOTTOM = (83, 166, 240, 255)


def folder_base(emblem=None):
    img = blank()
    back = rounded_mask((24, 52, 232, 214), 14)
    tab = rounded_mask((24, 38, 108, 80), 12)
    back_layer = Image.new("RGBA", (S, S), (70, 150, 228, 255))
    img.paste(back_layer, (0, 0), Image.composite(Image.new("L", (S, S), 255), back, tab))
    img.paste(back_layer, (0, 0), back)
    front_mask = rounded_mask((24, 74, 232, 220), 14)
    front = gradient((S, S), FOLDER_TOP, FOLDER_BOTTOM)
    img.paste(front, (0, 0), front_mask)
    d = ImageDraw.Draw(img)
    d.line([(36, 78), (220, 78)], fill=(190, 228, 255, 255), width=3)
    if emblem:
        e = blank()
        emblem(e, ImageDraw.Draw(e))
        tint = Image.new("RGBA", (S, S), (54, 128, 205, 255))
        tint.putalpha(e.getchannel("A").point(lambda v: v * 190 // 255))
        img.alpha_composite(tint)
    return drop_shadow(img, img.getchannel("A"), 4, 5, 60)


def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


# ---------------------------------------------------------------- app icons

def files_icon():
    def glyph(img, d):
        rr(d, (64, 92, 192, 178), 14, fill=(255, 255, 255, 255))
        rr(d, (64, 78, 120, 104), 10, fill=(255, 255, 255, 255))
        d.line([(76, 112), (180, 112)], fill=(30, 125, 240, 160), width=6)
    return app_base((112, 200, 255, 255), (22, 118, 240, 255), glyph)


def terminal_icon():
    def glyph(img, d):
        rr(d, (48, 56, 208, 200), 14, fill=(24, 24, 26, 255), outline=(0, 0, 0, 120), width=3)
        f = font(MONO_BOLD, 66)
        d.text((64, 66), ">_", font=f, fill=(235, 235, 240, 255))
    return app_base((214, 214, 220, 255), (150, 150, 158, 255), glyph)


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
    def glyph(img, d):
        gear_shape(d, 128, 128, 82, 66, 18, (226, 226, 232, 255))
        d.ellipse([70, 70, 186, 186], fill=(152, 152, 160, 255))
        gear_shape(d, 128, 128, 50, 38, 10, (234, 234, 240, 255))
        d.ellipse([110, 110, 146, 146], fill=(120, 120, 128, 255))
    return app_base((146, 146, 154, 255), (90, 90, 98, 255), glyph)


def calc_icon():
    def glyph(img, d):
        rr(d, (60, 50, 196, 92), 10, fill=(70, 70, 74, 255))
        f = font(INTER_BOLD, 34)
        d.text((150, 52), "42", font=f, fill=(240, 240, 240, 255))
        for row in range(3):
            for col in range(4):
                x, y = 66 + col * 34, 108 + row * 34
                color = (255, 159, 10, 255) if col == 3 else (165, 165, 170, 255) if row == 0 else (92, 92, 96, 255)
                d.ellipse([x, y, x + 26, y + 26], fill=color)
    return app_base((60, 60, 64, 255), (28, 28, 30, 255), glyph)


def notepad_icon():
    def glyph(img, d):
        rr(d, (62, 44, 194, 212), 8, fill=(255, 255, 255, 255))
        for i in range(7):
            y = 70 + i * 18
            d.line([(78, y), (178 - (i % 3) * 18, y)], fill=(150, 160, 175, 255), width=5)
        d.line([(150, 196), (206, 84)], fill=(255, 176, 32, 255), width=16)
        d.polygon([(142, 212), (144, 192), (156, 198)], fill=(60, 60, 60, 255))
    return app_base((190, 215, 245, 255), (120, 160, 220, 255), glyph)


def paint_icon():
    def glyph(img, d):
        d.ellipse([46, 70, 206, 196], fill=(245, 222, 186, 255))
        d.ellipse([134, 148, 164, 176], fill=(0, 0, 0, 0))
        for (x, y, c) in [(74, 104, (255, 59, 48)), (108, 86, (255, 204, 0)), (148, 88, (52, 199, 89)),
                          (176, 112, (0, 122, 255)), (80, 140, (175, 82, 222))]:
            d.ellipse([x, y, x + 26, y + 26], fill=c + (255,))
        d.line([(120, 182), (200, 52)], fill=(120, 80, 40, 255), width=12)
        d.polygon([(110, 200), (114, 176), (130, 186)], fill=(30, 30, 30, 255))
    return app_base((255, 255, 255, 255), (220, 224, 232, 255), glyph)


def monitor_icon():
    def glyph(img, d):
        rr(d, (50, 58, 206, 198), 12, fill=(10, 14, 12, 255))
        for i in range(1, 4):
            d.line([(50, 58 + i * 35), (206, 58 + i * 35)], fill=(40, 70, 50, 255), width=2)
        pts = [(56, 168), (84, 150), (106, 160), (128, 104), (150, 130), (172, 88), (200, 110)]
        d.line(pts, fill=(52, 230, 110, 255), width=7, joint="curve")
    return app_base((180, 180, 188, 255), (110, 110, 118, 255), glyph)


def mines_icon():
    def glyph(img, d):
        for row in range(4):
            for col in range(4):
                x, y = 46 + col * 42, 46 + row * 42
                c = (120, 200, 90, 255) if (row + col) % 2 else (100, 182, 74, 255)
                d.rectangle([x, y, x + 42, y + 42], fill=c)
        cx, cy = 118, 140
        for a in range(0, 360, 45):
            r = math.radians(a)
            d.line([(cx, cy), (cx + 46 * math.cos(r), cy + 46 * math.sin(r))], fill=(25, 25, 30, 255), width=10)
        d.ellipse([cx - 34, cy - 34, cx + 34, cy + 34], fill=(25, 25, 30, 255))
        d.ellipse([cx - 20, cy - 20, cx - 4, cy - 4], fill=(230, 230, 240, 255))
        d.line([(178, 60), (178, 128)], fill=(40, 40, 40, 255), width=6)
        d.polygon([(181, 60), (214, 76), (181, 92)], fill=(255, 59, 48, 255))
    return app_base((140, 210, 110, 255), (60, 150, 60, 255), glyph)


def launchpad_icon():
    def glyph(img, d):
        colors = [(255, 69, 58), (255, 159, 10), (255, 214, 10), (48, 209, 88), (100, 210, 255), (10, 132, 255),
                  (191, 90, 242), (255, 55, 95), (172, 142, 104)]
        for i, c in enumerate(colors):
            x, y = 60 + (i % 3) * 48, 60 + (i // 3) * 48
            rr(d, (x, y, x + 38, y + 38), 10, fill=c + (255,))
    return app_base((80, 80, 90, 255), (40, 40, 48, 255), glyph)


def logo_glyph(d, cx, cy, r, color=None):
    for y in range(int(cy - r), int(cy + r) + 1):
        t = (y - (cy - r)) / (2 * r)
        rel = y - cy
        if rel > -r * 0.15:
            period = max(6, r / 3.2)
            gap = 2 + (rel + r * 0.15) / (r * 1.15) * (period * 0.55)
            if (rel + r) % period < gap:
                continue
        half = math.sqrt(max(0.0, r * r - rel * rel))
        c = color or (int(255), int(230 - 170 * t), int(90 + 70 * t), 255)
        d.line([(cx - half, y), (cx + half, y)], fill=c)


def logo_icon():
    def glyph(img, d):
        logo_glyph(d, 128, 128, 72)
    return app_base((44, 22, 92, 255), (14, 6, 40, 255), glyph)


def computer_icon():
    img = blank()
    mask = rounded_mask((30, 72, 226, 190), 18)
    img.paste(gradient((S, S), (238, 238, 242, 255), (176, 176, 184, 255)), (0, 0), mask)
    d = ImageDraw.Draw(img)
    rr(d, (30, 72, 226, 190), 18, outline=(0, 0, 0, 50), width=3)
    d.line([(44, 150), (212, 150)], fill=(150, 150, 158, 255), width=3)
    d.ellipse([186, 160, 200, 174], fill=(52, 199, 89, 255))
    for i in range(6):
        d.line([(56 + i * 14, 162), (56 + i * 14, 176)], fill=(140, 140, 148, 255), width=4)
    return drop_shadow(img, img.getchannel("A"), 5, 6, 80)


def usb_icon():
    img = blank()
    mask = rounded_mask((40, 64, 216, 196), 20)
    img.paste(gradient((S, S), (255, 160, 60, 255), (230, 110, 20, 255)), (0, 0), mask)
    d = ImageDraw.Draw(img)
    rr(d, (40, 64, 216, 196), 20, outline=(0, 0, 0, 50), width=3)
    cx, cy = 128, 130
    d.line([(cx, cy - 40), (cx, cy + 40)], fill=(255, 255, 255, 255), width=8)
    d.polygon([(cx - 14, cy - 34), (cx + 14, cy - 34), (cx, cy - 56)], fill=(255, 255, 255, 255))
    d.line([(cx, cy + 6), (cx - 30, cy - 14), (cx - 30, cy - 26)], fill=(255, 255, 255, 255), width=7)
    d.line([(cx, cy + 20), (cx + 30, cy), (cx + 30, cy - 12)], fill=(255, 255, 255, 255), width=7)
    d.ellipse([cx - 12, cy + 34, cx + 12, cy + 58], fill=(255, 255, 255, 255))
    return drop_shadow(img, img.getchannel("A"), 5, 6, 80)


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


def exe_icon():
    def glyph(img, d):
        rr(d, (54, 62, 202, 194), 14, fill=(250, 250, 252, 255))
        d.rectangle([54, 62, 202, 92], fill=(225, 228, 236, 255))
        for i, c in enumerate([(255, 95, 87), (254, 188, 46), (40, 200, 64)]):
            d.ellipse([66 + i * 22, 70, 80 + i * 22, 84], fill=c + (255,))
        d.rectangle([70, 110, 130, 124], fill=(120, 150, 220, 255))
        d.rectangle([70, 138, 186, 146], fill=(190, 200, 220, 255))
        d.rectangle([70, 158, 170, 166], fill=(190, 200, 220, 255))
    return app_base((120, 140, 170, 255), (70, 86, 120, 255), glyph)


def circle_badge(c_top, c_bottom, draw):
    img = blank()
    mask = Image.new("L", (S * 4, S * 4), 0)
    ImageDraw.Draw(mask).ellipse([30 * 4, 30 * 4, 226 * 4, 226 * 4], fill=255)
    mask = mask.resize((S, S), Image.LANCZOS)
    img.paste(gradient((S, S), c_top, c_bottom), (0, 0), mask)
    draw(img, ImageDraw.Draw(img))
    return drop_shadow(img, mask, 4, 6, 70)


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
    img.paste(gradient((S, S), (255, 214, 10, 255), (255, 170, 0, 255)), (0, 0), mask)
    d = ImageDraw.Draw(img)
    rr(d, (118, 92, 138, 168), 8, fill=(30, 30, 30, 255))
    d.ellipse([116, 178, 140, 202], fill=(30, 30, 30, 255))
    return drop_shadow(img, mask, 4, 6, 70)


def user_icon():
    def draw(img, d):
        d.ellipse([88, 62, 168, 142], fill=(255, 255, 255, 255))
        d.ellipse([52, 150, 204, 290], fill=(255, 255, 255, 255))
    img = blank()
    mask = Image.new("L", (S * 4, S * 4), 0)
    ImageDraw.Draw(mask).ellipse([16 * 4, 16 * 4, 240 * 4, 240 * 4], fill=255)
    mask = mask.resize((S, S), Image.LANCZOS)
    body = gradient((S, S), (176, 180, 190, 255), (128, 132, 142, 255))
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
    ImageDraw.Draw(body).polygon([(60 * 4, 60 * 4), (196 * 4, 60 * 4), (182 * 4, 232 * 4), (74 * 4, 232 * 4)], fill=255)
    body = body.resize((S, S), Image.LANCZOS)
    img.paste(gradient((S, S), (238, 240, 244, 230), (196, 200, 210, 230)), (0, 0), body)
    d = ImageDraw.Draw(img)
    for i in range(7):
        x = 78 + i * 17
        d.line([(x, 70), (x + (i - 3) * 2, 226)], fill=(150, 156, 170, 255), width=4)
    if full:
        for (x, y, c) in [(80, 36, (255, 255, 255)), (120, 30, (250, 245, 220)), (150, 42, (230, 240, 255))]:
            rr(d, (x, y, x + 46, y + 40), 4, fill=c + (255,), outline=(180, 180, 190, 255), width=2)
    rr(d, (50, 50, 206, 70), 8, fill=(220, 224, 232, 255), outline=(140, 146, 160, 255), width=3)
    return drop_shadow(img, img.getchannel("A"), 4, 6, 60)


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
]

# ---------------------------------------------------------------- symbols (drawn on a 128 grid)

W = 9


def sym_logo(d):
    d.ellipse([10, 10, 118, 118], fill=255)
    for y, t in ((62, 9), (80, 11), (98, 13)):
        d.rectangle([0, y, 128, y + t], fill=0)


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
    entries = []
    for name, fn in ICONS:
        img = fn().resize((SIZE, SIZE), Image.LANCZOS)
        packed = lz4pack.compress(lz4pack.planes(argb(img), SIZE))
        var = name.lower() + "_lz"
        emit_bytes(var, packed)
        entries.append((name, var, len(packed)))
    print("const struct packed_image icon_pack[ICON_COUNT] = {")
    for name, var, n in entries:
        print(f"    [{name}] = {{{SIZE}, {n}, {var}}},")
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
