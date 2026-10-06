#!/usr/bin/env python3
"""Generates the SynthOS 8x16 console font (CP437 and CP866) as C source.

Letters come from DejaVu Sans Mono; box drawing, block and shade characters
are drawn procedurally so they join seamlessly between cells.
usage: tools/mkfont.py [font.ttf] > kernel/core/font8x16.c (needs Pillow)
"""
import sys
import unicodedata
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"

CP437_LOW = ("\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼")


def codepage(name):
    table = []
    for b in range(256):
        if b < 32:
            table.append(CP437_LOW[b] if b else " ")
        elif b == 0x7F:
            table.append("⌂")
        else:
            table.append(bytes([b]).decode(name))
    return table


def box_arms(ch):
    name = unicodedata.name(ch, "")
    if not name.startswith("BOX DRAWINGS") or any(k in name for k in ("ARC", "DIAGONAL", "DASH", "HEAVY")):
        return None
    words = name.replace("BOX DRAWINGS ", "").split()
    arms = {"UP": 0, "DOWN": 0, "LEFT": 0, "RIGHT": 0}
    default = 2 if words[0] == "DOUBLE" else 1
    for j, w in enumerate(words):
        if w not in ("VERTICAL", "HORIZONTAL", "UP", "DOWN", "LEFT", "RIGHT"):
            continue
        weight = default
        if j + 1 < len(words) and words[j + 1] in ("SINGLE", "DOUBLE"):
            weight = 2 if words[j + 1] == "DOUBLE" else 1
        group = ["UP", "DOWN"] if w == "VERTICAL" else ["LEFT", "RIGHT"] if w == "HORIZONTAL" else [w]
        for d in group:
            arms[d] = weight
    return arms if any(arms.values()) else None


def box_glyph(ch):
    arms = box_arms(ch)
    if not arms:
        return None
    up, down, left, right = arms["UP"], arms["DOWN"], arms["LEFT"], arms["RIGHT"]
    img = [[0] * W for _ in range(H)]
    cx, cy = 3, 7
    hd = left == 2 or right == 2
    vd = up == 2 or down == 2

    def vline(x, y0, y1):
        for y in range(max(0, min(y0, y1)), min(H, max(y0, y1) + 1)):
            img[y][x] = 1

    def hline(y, x0, x1):
        for x in range(max(0, min(x0, x1)), min(W, max(x0, x1) + 1)):
            img[y][x] = 1

    def stop(side_weight, near, mid, far):
        return near if side_weight == 2 else mid if side_weight == 1 else far

    if up == 1:
        vline(cx, 0, cy + 1 if hd else cy)
    if down == 1:
        vline(cx, H - 1, cy - 1 if hd else cy)
    if left == 1:
        hline(cy, 0, cx + 1 if vd else cx)
    if right == 1:
        hline(cy, W - 1, cx - 1 if vd else cx)
    if up == 2:
        vline(cx - 1, 0, stop(left, cy - 1, cy, cy + 1))
        vline(cx + 1, 0, stop(right, cy - 1, cy, cy + 1))
    if down == 2:
        vline(cx - 1, H - 1, stop(left, cy + 1, cy, cy - 1))
        vline(cx + 1, H - 1, stop(right, cy + 1, cy, cy - 1))
    if left == 2:
        hline(cy - 1, 0, stop(up, cx - 1, cx, cx + 1))
        hline(cy + 1, 0, stop(down, cx - 1, cx, cx + 1))
    if right == 2:
        hline(cy - 1, W - 1, stop(up, cx + 1, cx, cx - 1))
        hline(cy + 1, W - 1, stop(down, cx + 1, cx, cx - 1))
    return img


def block_glyph(ch):
    o = ord(ch)
    full = [[1] * W for _ in range(H)]
    if o == 0x2588:
        return full
    if o == 0x2580:
        return [[1] * W if y < H // 2 else [0] * W for y in range(H)]
    if o == 0x2584:
        return [[1] * W if y >= H // 2 else [0] * W for y in range(H)]
    if o == 0x258C:
        return [[1 if x < W // 2 else 0 for x in range(W)] for _ in range(H)]
    if o == 0x2590:
        return [[1 if x >= W // 2 else 0 for x in range(W)] for _ in range(H)]
    if o in (0x2591, 0x2592, 0x2593):
        g = []
        for y in range(H):
            row = []
            for x in range(W):
                if o == 0x2591:
                    v = (x + (y % 4) * 2) % 4 == 0 and y % 2 == 0
                elif o == 0x2592:
                    v = (x + y) % 2 == 0
                else:
                    v = not ((x + (y % 4) * 2) % 4 == 0 and y % 2 == 0)
                row.append(1 if v else 0)
            g.append(row)
        return g
    if o == 0x25A0:
        return [[1 if 1 <= x <= 6 and 4 <= y <= 11 else 0 for x in range(W)] for y in range(H)]
    return None


def render(ch, font):
    for gen in (box_glyph, block_glyph):
        g = gen(ch)
        if g:
            return g
    if ch == " ":
        return [[0] * W for _ in range(H)]
    img = Image.new("L", (W * 4, H * 4), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=font, fill=255)
    big = img
    small = big.resize((W, H), Image.BOX)
    px = small.load()
    out = []
    for y in range(H):
        out.append([1 if px[x, y] >= 96 else 0 for x in range(W)])
    return out


def main():
    font = ImageFont.truetype(TTF, 13 * 4)
    tables = {}
    for cp in ("cp437", "cp866"):
        glyphs = []
        for ch in codepage(cp):
            g = render(ch, font)
            glyphs.append(bytes(sum(bit << (7 - x) for x, bit in enumerate(row)) for row in g))
        tables[cp] = glyphs
    print("/* Generated by tools/mkfont.py from DejaVu Sans Mono (Bitstream Vera license). Do not edit. */")
    print("#include <synth/font.h>\n")
    for cp, glyphs in tables.items():
        print(f"const uint8_t font8x16_{cp}[256][16] = {{")
        for i, g in enumerate(glyphs):
            print("    {" + ", ".join(f"0x{b:02x}" for b in g) + f"}}, /* {i:02x} */")
        print("};\n")


main()
