#!/usr/bin/env python3
"""Packs a program from a zip archive into a SynthOS bundle, the area between the system and the data partition.

usage: tools/mkbundle.py archive.zip out.bundle "Title" prefix/ [skip-pattern ...]

The bundle starts with a 512-byte header ("SYNTHBDL", version, total sectors, entry count, data bytes, title,
the prefix to strip), then one record per file: "SBE1", name length, method (0 stored, 8 deflate), CRC-32,
compressed and plain sizes, the name and the data copied from the zip as it is. The kernel unpacks it on the
first boot (kernel/core/bundle.c), so the image needs no network to have the program.
"""
import struct
import sys
import zipfile


def main():
    src, out, title, prefix = sys.argv[1:5]
    skips = sys.argv[5:]
    z = zipfile.ZipFile(src)
    raw = open(src, 'rb')
    records, count, data_bytes = [], 0, 0
    for info in z.infolist():
        name = info.filename
        if name.endswith('/') or not name.startswith(prefix) or any(s in name for s in skips):
            continue
        if info.compress_type not in (0, 8):
            sys.exit(f'{name}: compression method {info.compress_type} is not supported')
        raw.seek(info.header_offset)
        local = raw.read(30)
        if local[:4] != b'PK\x03\x04':
            sys.exit(f'{name}: bad local header')
        name_len, extra_len = struct.unpack('<HH', local[26:30])
        raw.seek(info.header_offset + 30 + name_len + extra_len)
        data = raw.read(info.compress_size)
        encoded = name.encode()
        records.append(struct.pack('<4sHBBIII', b'SBE1', len(encoded), info.compress_type, 0, info.CRC,
                                   info.compress_size, info.file_size) + encoded)
        records.append(data)
        count += 1
        data_bytes += info.file_size
    body = b''.join(records)
    sectors = 1 + (len(body) + 511) // 512
    sectors = (sectors + 2047) // 2048 * 2048
    header = struct.pack('<8sIIIQ', b'SYNTHBDL', 1, sectors, count, data_bytes)
    header += title.encode()[:63].ljust(64, b'\0') + prefix.encode()[:63].ljust(64, b'\0')
    with open(out, 'wb') as f:
        f.write(header.ljust(512, b'\0'))
        f.write(body)
        f.truncate(sectors * 512)
    print(f'  BUNDLE  {out} ({count} files, {data_bytes >> 20} MB unpacked, {sectors >> 11} MB)')


main()
