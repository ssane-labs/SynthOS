#!/usr/bin/env python3
"""Packs the ramdisk with LZMA: "SRDL", the unpacked size, the 5 property bytes, the raw stream
(decoder: kernel/lib/lzma.c). usage: tools/lzmapack.py <raw pack> <output>"""
import lzma
import struct
import sys

LC, LP, PB, DICT = 3, 0, 2, 1 << 22


def main():
    raw = open(sys.argv[1], "rb").read()
    filters = [{"id": lzma.FILTER_LZMA1, "dict_size": DICT, "lc": LC, "lp": LP, "pb": PB,
                "preset": 9 | lzma.PRESET_EXTREME}]
    packed = lzma.compress(raw, format=lzma.FORMAT_RAW, filters=filters)
    props = bytes([(PB * 5 + LP) * 9 + LC]) + struct.pack("<I", DICT)
    with open(sys.argv[2], "wb") as f:
        f.write(b"SRDL" + struct.pack("<I", len(raw)) + props + packed)
    print(f"  LZMA    {len(raw)} -> {len(packed) + 13} bytes")


main()
