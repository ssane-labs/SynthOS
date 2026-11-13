#!/usr/bin/env python3
"""Pictures for the Photos library tests: two of the same drawn face, a red landscape and a blue portrait."""
import struct
import sys
import zlib


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def face(cx, cy, back):
    def pixel(x, y):
        dx, dy = (x - cx) / 30, (y - cy) / 40
        if dx * dx + dy * dy > 1:
            return back
        for ex in (cx - 12, cx + 12):  # eyes
            if (x - ex) ** 2 + (y - (cy - 10)) ** 2 <= 16:
                return (40, 30, 30)
        if abs(x - cx) < 10 and cy + 18 <= y <= cy + 21:  # mouth
            return (120, 50, 50)
        return (224, 172, 140)
    return pixel


out = sys.argv[1]
png(out + "/face.png", 160, 120, face(70, 60, (30, 60, 160)))
png(out + "/face2.png", 160, 120, face(95, 62, (40, 130, 60)))
png(out + "/red.png", 120, 80, lambda x, y: (220, 30, 30))
png(out + "/tall.png", 60, 120, lambda x, y: (20, 40, 200) if y > 20 else (250, 250, 250))
