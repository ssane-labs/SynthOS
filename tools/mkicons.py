#!/usr/bin/env python3
"""Draws the SynthOS desktop icons (32x32 and 16x16 ARGB) and writes them as C source.

usage: tools/mkicons.py > kernel/gui/icons.c (needs Pillow)
"""
import math
from PIL import Image, ImageDraw, ImageFont

S = 128
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def canvas():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def vgrad(img, box, top, bottom, radius=0):
    x0, y0, x1, y1 = box
    grad = Image.new("RGBA", (x1 - x0, y1 - y0))
    gd = ImageDraw.Draw(grad)
    for y in range(y1 - y0):
        t = y / max(1, y1 - y0 - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4))
        gd.line([(0, y), (x1 - x0, y)], fill=c)
    mask = Image.new("L", grad.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, grad.size[0] - 1, grad.size[1] - 1], radius=radius, fill=255)
    img.paste(grad, (x0, y0), mask)


def page(img, d, color=(250, 250, 255, 255), fold=(200, 205, 220, 255)):
    vgrad(img, (22, 8, 106, 120), color, (225, 228, 240, 255), 8)
    d.polygon([(80, 8), (106, 34), (80, 34)], fill=fold)
    d.rounded_rectangle([22, 8, 105, 119], radius=8, outline=(120, 125, 150, 255), width=3)


def folder():
    img, d = canvas()
    vgrad(img, (8, 20, 60, 44), (240, 170, 40, 255), (230, 150, 30, 255), 8)
    vgrad(img, (8, 32, 120, 112), (255, 205, 80, 255), (240, 160, 40, 255), 10)
    d.rounded_rectangle([8, 32, 119, 111], radius=10, outline=(190, 120, 20, 255), width=3)
    d.line([(14, 44), (114, 44)], fill=(255, 230, 150, 255), width=3)
    return img


def text_file():
    img, d = canvas()
    page(img, d)
    for i, w in enumerate([60, 70, 52, 66, 40]):
        y = 46 + i * 14
        d.line([(34, y), (34 + w, y)], fill=(110, 120, 150, 255), width=5)
    return img


def blank_file():
    img, d = canvas()
    page(img, d)
    return img


def window_app():
    img, d = canvas()
    vgrad(img, (8, 16, 120, 112), (70, 130, 240, 255), (40, 80, 200, 255), 12)
    d.rounded_rectangle([16, 38, 112, 104], radius=6, fill=(235, 242, 255, 255))
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse([20 + i * 16, 22, 30 + i * 16, 32], fill=c + (255,))
    d.rectangle([26, 50, 70, 60], fill=(120, 150, 220, 255))
    d.rectangle([26, 68, 100, 74], fill=(170, 190, 230, 255))
    d.rectangle([26, 82, 90, 88], fill=(170, 190, 230, 255))
    return img


def gear(color=(150, 160, 185, 255), hole=(0, 0, 0, 0), dark=(90, 100, 125, 255)):
    img, d = canvas()
    cx = cy = 64
    pts = []
    for i in range(16):
        a = i * math.pi / 8
        r = 56 if i % 2 == 0 else 44
        for da in (-0.17, 0.17):
            pts.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    d.polygon(pts, fill=color, outline=dark)
    d.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], fill=color, outline=dark, width=3)
    d.ellipse([cx - 18, cy - 18, cx + 18, cy + 18], fill=(40, 44, 60, 255))
    return img


def image_file():
    img, d = canvas()
    vgrad(img, (8, 18, 120, 110), (120, 190, 255, 255), (200, 230, 255, 255), 10)
    d.ellipse([78, 30, 100, 52], fill=(255, 220, 60, 255))
    d.polygon([(8, 110), (46, 56), (78, 110)], fill=(60, 160, 90, 255))
    d.polygon([(48, 110), (84, 66), (120, 110)], fill=(40, 130, 70, 255))
    d.rounded_rectangle([8, 18, 119, 109], radius=10, outline=(70, 90, 130, 255), width=3)
    return img


def terminal():
    img, d = canvas()
    vgrad(img, (6, 14, 122, 114), (52, 56, 78, 255), (24, 26, 38, 255), 14)
    d.rounded_rectangle([6, 14, 121, 113], radius=14, outline=(100, 110, 140, 255), width=3)
    d.line([(26, 44), (50, 62), (26, 80)], fill=(80, 250, 140, 255), width=9, joint="curve")
    d.line([(58, 86), (96, 86)], fill=(230, 240, 255, 255), width=8)
    return img


def calculator():
    img, d = canvas()
    vgrad(img, (22, 6, 106, 122), (90, 96, 120, 255), (55, 60, 80, 255), 12)
    d.rounded_rectangle([32, 16, 96, 42], radius=4, fill=(190, 240, 200, 255))
    colors = [(230, 232, 240), (230, 232, 240), (255, 150, 70)]
    for r in range(4):
        for c in range(3):
            x, y = 32 + c * 23, 52 + r * 17
            d.rounded_rectangle([x, y, x + 17, y + 12], radius=3, fill=colors[c] + (255,))
    return img


def notepad():
    img, d = canvas()
    vgrad(img, (18, 12, 104, 120), (255, 248, 200, 255), (250, 235, 160, 255), 8)
    d.rounded_rectangle([18, 12, 103, 119], radius=8, outline=(180, 150, 70, 255), width=3)
    for i in range(5):
        d.line([(28, 38 + i * 15), (94, 38 + i * 15)], fill=(150, 170, 220, 255), width=3)
    d.polygon([(70, 100), (112, 40), (124, 50), (82, 110), (66, 116)], fill=(250, 120, 80, 255))
    d.polygon([(66, 116), (70, 100), (82, 110)], fill=(60, 60, 70, 255))
    return img


def paint():
    img, d = canvas()
    d.ellipse([8, 16, 120, 112], fill=(240, 220, 190, 255), outline=(170, 140, 100, 255), width=3)
    d.ellipse([70, 66, 92, 88], fill=(0, 0, 0, 0))
    for (x, y), c in zip([(28, 40), (52, 28), (80, 34), (30, 68), (52, 86)],
                         [(240, 60, 60), (250, 200, 40), (60, 170, 250), (70, 200, 90), (180, 80, 220)]):
        d.ellipse([x, y, x + 18, y + 18], fill=c + (255,))
    return img


def computer():
    img, d = canvas()
    vgrad(img, (8, 14, 120, 90), (90, 100, 130, 255), (60, 66, 90, 255), 8)
    vgrad(img, (16, 22, 112, 82), (60, 200, 255, 255), (150, 90, 255, 255), 4)
    d.rectangle([54, 90, 74, 104], fill=(80, 86, 110, 255))
    d.rounded_rectangle([30, 104, 98, 114], radius=4, fill=(100, 108, 135, 255))
    return img


def home():
    img, d = canvas()
    d.polygon([(64, 10), (122, 62), (108, 62), (108, 116), (20, 116), (20, 62), (6, 62)], fill=(250, 250, 255, 255),
              outline=(110, 120, 150, 255))
    d.polygon([(64, 8), (124, 62), (112, 66), (64, 24), (16, 66), (4, 62)], fill=(230, 80, 90, 255))
    d.rounded_rectangle([52, 78, 76, 116], radius=4, fill=(120, 80, 50, 255))
    d.rectangle([28, 74, 44, 90], fill=(120, 190, 255, 255))
    return img


def monitor():
    img, d = canvas()
    vgrad(img, (8, 14, 120, 114), (40, 44, 62, 255), (24, 26, 38, 255), 12)
    pts = [(16, 90), (34, 70), (50, 80), (68, 42), (84, 60), (100, 30), (112, 50)]
    d.line(pts, fill=(80, 250, 160, 255), width=6, joint="curve")
    for x in range(16, 116, 20):
        d.line([(x, 24), (x, 104)], fill=(60, 66, 90, 255), width=1)
    return img


def mine():
    img, d = canvas()
    vgrad(img, (4, 4, 124, 124), (170, 200, 230, 255), (110, 140, 190, 255), 22)
    for a in range(0, 360, 45):
        r = math.radians(a)
        d.line([(64, 64), (64 + 48 * math.cos(r), 64 + 48 * math.sin(r))], fill=(30, 30, 40, 255), width=10)
    d.ellipse([30, 30, 98, 98], fill=(30, 32, 44, 255))
    d.ellipse([44, 44, 60, 60], fill=(230, 230, 240, 255))
    return img


def dos():
    img, d = canvas()
    vgrad(img, (6, 18, 122, 110), (30, 30, 170, 255), (10, 10, 120, 255), 10)
    font = ImageFont.truetype(MONO, 34)
    d.text((16, 44), "C:\\>", font=font, fill=(240, 240, 240, 255))
    return img


def sx():
    img, d = canvas()
    d.polygon([(64, 6), (122, 64), (64, 122), (6, 64)], fill=(170, 80, 255, 255), outline=(110, 40, 190, 255))
    font = ImageFont.truetype(SANS, 38)
    d.text((32, 42), "SX", font=font, fill=(255, 255, 255, 255))
    return img


def script():
    img, d = canvas()
    page(img, d, (235, 255, 240, 255))
    font = ImageFont.truetype(MONO, 46)
    d.text((40, 42), "$_", font=font, fill=(40, 150, 80, 255))
    return img


def power():
    img, d = canvas()
    d.ellipse([10, 10, 118, 118], fill=(235, 70, 90, 255))
    d.arc([34, 34, 94, 94], start=-60, end=240, fill=(255, 255, 255, 255), width=10)
    d.line([(64, 24), (64, 64)], fill=(255, 255, 255, 255), width=10)
    return img


def logo():
    img, d = canvas()
    vgrad(img, (10, 10, 118, 118), (255, 120, 200, 255), (255, 210, 60, 255), 54)
    for i, y in enumerate([72, 86, 100]):
        d.rectangle([0, y, S, y + 6 - i], fill=(0, 0, 0, 0))
    return img


def info():
    img, d = canvas()
    d.ellipse([8, 8, 120, 120], fill=(60, 140, 255, 255))
    d.ellipse([56, 26, 72, 42], fill=(255, 255, 255, 255))
    d.rounded_rectangle([56, 52, 72, 102], radius=6, fill=(255, 255, 255, 255))
    return img


def dll():
    return gear((120, 200, 255, 255), dark=(60, 120, 190, 255))


ICONS = [
    ("ICON_FOLDER", folder), ("ICON_FILE", blank_file), ("ICON_TEXT", text_file), ("ICON_APP", window_app),
    ("ICON_DLL", dll), ("ICON_IMAGE", image_file), ("ICON_TERMINAL", terminal), ("ICON_SETTINGS", gear),
    ("ICON_CALC", calculator), ("ICON_NOTEPAD", notepad), ("ICON_PAINT", paint), ("ICON_COMPUTER", computer),
    ("ICON_HOME", home), ("ICON_MONITOR", monitor), ("ICON_MINES", mine), ("ICON_DOS", dos), ("ICON_SX", sx),
    ("ICON_SCRIPT", script), ("ICON_POWER", power), ("ICON_LOGO", logo), ("ICON_INFO", info),
]


def pixels(img, size):
    small = img.resize((size, size), Image.LANCZOS)
    px = small.load()
    out = []
    for y in range(size):
        for x in range(size):
            r, g, b, a = px[x, y]
            out.append(a << 24 | r << 16 | g << 8 | b if a else 0)
    return out


def main():
    print("/* Generated by tools/mkicons.py. Do not edit. */")
    print("#include <synth/gui.h>\n")
    names = []
    for name, fn in ICONS:
        img = fn()
        for size in (32, 16):
            data = pixels(img, size)
            var = f"{name.lower()}_{size}"
            print(f"static const uint32_t {var}[{size * size}] = {{")
            for i in range(0, len(data), 8):
                print("    " + ", ".join(f"0x{v:08x}" for v in data[i:i + 8]) + ",")
            print("};\n")
        names.append(name)
    print("const struct icon icons[ICON_COUNT] = {")
    for name in names:
        print(f"    [{name}] = {{{name.lower()}_32, {name.lower()}_16}},")
    print("};")


main()
