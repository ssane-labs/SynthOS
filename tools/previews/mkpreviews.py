#!/usr/bin/env python3
"""Packs screen recordings of SynthOS into rootfs/system/previews.pak, the clips Settings and Setup play.

usage: tools/previews/mkpreviews.py FRAMES_DIR   (frames recorded by tools/previews/record.py)

A clip is cut out of the recorded screens (a crop box, scaled to the clip's size); its segments follow each other
with a short crossfade. Every frame stores only the rectangle that changed since the frame before it, as a baseline
JPEG; the first frame is whole.

previews.pak: "SPV1", u32 count, count x {char name[24], u32 offset, u32 size}, then the clips.
clip: u16 w, u16 h, u16 fps, u16 frames, then per frame: u16 x, y, w, h, u32 length, JPEG (length 0: no change).
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from clips import CLIPS  # noqa: E402

FADE = 5
THRESHOLD = 10  # a pixel that moved this much since the frame before changed
DRIFT = 48      # or that is this far from what the player shows (JPEG error alone stays below)
QUALITY = 72


def frames_of(d, crop, size, segments):
    out = []
    for si in range(segments):
        names = sorted(n for n in os.listdir(d) if n.startswith(f"{si}_") and n.endswith(".png"))
        seg = [Image.open(os.path.join(d, n)).convert("RGB").crop(crop).resize(size, Image.LANCZOS) for n in names]
        if out and seg:
            for i in range(min(FADE, len(seg))):
                out[-FADE + i] = Image.blend(out[-FADE + i], seg[i], (i + 1) / (FADE + 1))
            seg = seg[FADE:]
        out += seg
    return out


def jpeg(img):
    b = io.BytesIO()
    img.save(b, "JPEG", quality=QUALITY, optimize=True)
    return b.getvalue()


def encode(frames, fps):
    w, h = frames[0].size
    data = bytearray(struct.pack("<4H", w, h, fps, len(frames)))
    recon = prev = None
    for f in frames:
        a = np.asarray(f, dtype=np.int16)
        if recon is None:
            box = (0, 0, w, h)
        else:
            diff = (np.abs(a - prev).max(axis=2) > THRESHOLD) | (np.abs(a - recon).max(axis=2) > DRIFT)
            prev = a
            ys, xs = np.nonzero(diff)
            if not len(xs):
                data += struct.pack("<4HI", 0, 0, 0, 0, 0)
                continue
            x0, x1 = xs.min() // 16 * 16, min(w, (xs.max() // 16 + 1) * 16)
            y0, y1 = ys.min() // 16 * 16, min(h, (ys.max() // 16 + 1) * 16)
            box = (x0, y0, x1, y1)
        part = jpeg(f.crop(box))
        back = np.asarray(Image.open(io.BytesIO(part)).convert("RGB"), dtype=np.int16)
        if recon is None:
            recon = back.copy()
            prev = a
        else:
            recon[box[1]:box[3], box[0]:box[2]] = back
        data += struct.pack("<4HI", box[0], box[1], box[2] - box[0], box[3] - box[1], len(part)) + part
    return bytes(data)


def main():
    src = sys.argv[1]
    clips = []
    made = {}
    for name, rec, crop, size, segments in CLIPS:
        if (rec, crop, size) in made:
            clips.append((name, made[(rec, crop, size)]))
            continue
        d = os.path.join(src, rec)
        if not os.path.isdir(d):
            print(f"  missing {rec}", file=sys.stderr)
            continue
        frames = frames_of(d, crop, size, segments)
        data = encode(frames, 10)
        clips.append((name, data))
        made[(rec, crop, size)] = data
        print(f"  {name:24} {len(frames):3} frames {len(data) // 1024:4} KB", file=sys.stderr)
    out = bytearray(b"SPV1" + struct.pack("<I", len(clips)))
    offset = 8 + len(clips) * 32
    placed = {}
    body = bytearray()
    for name, data in clips:
        if id(data) not in placed:  # a clip under two names is stored once
            placed[id(data)] = offset + len(body)
            body += data
        out += name.encode().ljust(24, b"\0")[:24] + struct.pack("<II", placed[id(data)], len(data))
    out += body
    pak = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "rootfs", "system", "previews.pak")
    with open(pak, "wb") as f:
        f.write(out)
    print(f"previews.pak: {len(clips)} clips, {len(out) // 1024} KB", file=sys.stderr)


main()
