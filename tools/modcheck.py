#!/usr/bin/env python3
"""Checks an app module (.sxm) the way kernel/gui/module.c loads it: every relocation is RELATIVE (the kernel's
symbols were resolved at link time) and no symbol is left undefined."""
import struct
import sys


def main(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF':
        sys.exit(f'{path}: not ELF')
    is64 = d[4] == 2
    if is64:
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shentsize, shnum = struct.unpack_from('<HH', d, 0x3a)
    else:
        shoff, = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum = struct.unpack_from('<HH', d, 0x2e)
    sections = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            name, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from('<IIQQQQIIQQ', d, o)
        else:
            name, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from('<IIIIIIIIII', d, o)
        sections.append((typ, off, size, link, entsize))
    bad = []
    for typ, off, size, link, entsize in sections:
        if typ in (4, 9) and entsize:  # RELA, REL
            for k in range(size // entsize):
                info, = struct.unpack_from('<Q' if is64 else '<I', d, off + k * entsize + (8 if is64 else 4))
                kind = info & 0xffffffff if is64 else info & 0xff
                if kind != 8:
                    bad.append(f'relocation type {kind}')
        if typ == 11 and entsize:  # DYNSYM
            strtab = sections[link]
            for k in range(1, size // entsize):
                o = off + k * entsize
                if is64:
                    name, _, _, shndx = struct.unpack_from('<IBBH', d, o)
                else:
                    name, = struct.unpack_from('<I', d, o)
                    shndx, = struct.unpack_from('<H', d, o + 14)
                if shndx == 0:
                    s = d[strtab[1] + name:d.index(b'\0', strtab[1] + name)].decode()
                    bad.append(f'undefined {s}')
    if bad:
        sys.exit(f'{path}: ' + ', '.join(sorted(set(bad))[:20]))


main(sys.argv[1])
