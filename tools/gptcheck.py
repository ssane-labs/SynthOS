#!/usr/bin/env python3
"""Checks a disk image's GPT (both copies, all checksums) and prints its partitions."""
import struct, sys, zlib

def header(img, lba):
    img.seek(lba * 512)
    h = img.read(512)
    if h[:8] != b"EFI PART":
        sys.exit(f"no GPT header at LBA {lba}")
    size = struct.unpack_from("<I", h, 12)[0]
    crc = struct.unpack_from("<I", h, 16)[0]
    if zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:size]) != crc:
        sys.exit(f"bad header checksum at LBA {lba}")
    my, alt, first, last = struct.unpack_from("<QQQQ", h, 24)
    entries, count, esize, ecrc = struct.unpack_from("<QIII", h, 72)
    img.seek(entries * 512)
    table = img.read(count * esize)
    if zlib.crc32(table) != ecrc:
        sys.exit(f"bad entry checksum for the header at LBA {lba}")
    if my != lba:
        sys.exit(f"header at LBA {lba} says it is at {my}")
    return alt, table, esize

with open(sys.argv[1], "rb") as img:
    alt, table, esize = header(img, 1)
    alt2, table2, _ = header(img, alt)
    if alt2 != 1 or table != table2:
        sys.exit("backup GPT differs from the primary one")
    for i in range(len(table) // esize):
        e = table[i * esize:(i + 1) * esize]
        if any(e[:16]):
            first, last = struct.unpack_from("<QQ", e, 32)
            name = e[56:128].decode("utf-16-le").rstrip("\0")
            print(f"{i + 1} {first} {last} {name}")
print("gpt ok")
