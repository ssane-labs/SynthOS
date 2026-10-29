#!/usr/bin/env python3
"""Lists the imports of Windows programs that SynthOS does not provide yet.

usage: tools/w32missing.py build/serial32/kernel.elf DIR_OR_FILES...

DLLs that ship next to the programs are resolved against their own exports; everything else is looked up in the
built-in modules of the kernel (with the same api-set aliases as kernel/win32/pe.c). Needs python3-pefile.
"""
import os, re, struct, subprocess, sys, collections
import pefile

elf = sys.argv[1]
data = open(elf, 'rb').read()
secs = []
for line in subprocess.run(['readelf', '-SW', elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
    if m:
        secs.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)))
def off(va):
    for n, a, o, s in secs:
        if a and a <= va < a + s and n != '.bss':
            return o + va - a
def u32(va): return struct.unpack('<I', data[off(va):off(va) + 4])[0]
def cstr(va):
    o = off(va); return data[o:data.index(b'\0', o)].decode()
syms = {}
for line in subprocess.run(['nm', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
builtin = collections.defaultdict(set)
a = syms['__start_synth_win32']
while a < syms['__stop_synth_win32']:
    d = u32(a); a += 4
    name, exp, count = cstr(u32(d)), u32(d + 4), u32(d + 8)
    for i in range(count):
        builtin[name.lower()].add(cstr(u32(exp + i * 8)))

SETS = [('api-ms-win-shcore-', 'shcore.dll'), ('api-ms-win-downlevel-shell32-', 'shell32.dll'),
        ('api-ms-win-downlevel-shlwapi-', 'shlwapi.dll'), ('api-ms-win-downlevel-user32-', 'user32.dll'),
        ('api-ms-win-downlevel-advapi32-', 'advapi32.dll'), ('api-ms-win-security-', 'advapi32.dll'),
        ('api-ms-win-power-', 'powrprof.dll'), ('api-ms-win-appmodel-', 'kernel32.dll'), ('psapi.dll', 'kernel32.dll')]
def alias(n):
    if n.startswith('api-ms-win-crt-') or n == 'ucrtbase.dll' or n.startswith('msvcr') or n.startswith('vcruntime'):
        return 'msvcrt.dll'
    if n.startswith('api-ms-win-core-') or n == 'kernelbase.dll':
        return 'kernel32.dll'
    for p, t in SETS:
        if n.startswith(p): return t
    return n

src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'kernel', 'win32', 'pe.c')).read()
for mod, name, tmod, tname in re.findall(r'\{"([\w.]+)", "(\w+)", "([\w.]+)", "(\w+)"\}', src):
    if tname in builtin.get(tmod, ()):
        builtin[mod].add(name)

files = []
for arg in sys.argv[2:]:
    if os.path.isdir(arg):
        for root, _, names in os.walk(arg):
            files += [os.path.join(root, f) for f in names if f.lower().endswith(('.exe', '.dll'))]
    else:
        files.append(arg)
local = {}
for f in files:
    try:
        pe = pefile.PE(f, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
        names = {e.name.decode() for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None).symbols if e.name} \
            if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else set()
        local[os.path.basename(f).lower()] = names
    except Exception:
        pass
missing = collections.defaultdict(set)
for f in files:
    try:
        pe = pefile.PE(f, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                                               pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
    except Exception:
        continue
    if pe.FILE_HEADER.Machine != 0x14c:
        continue
    for entry in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) + getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
        dll = entry.dll.decode().lower()
        if dll in local:
            continue
        mod = alias(dll)
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else '#%d' % imp.ordinal
            have = builtin.get(mod, set())
            if mod == 'kernel32.dll':
                have = have | builtin.get('advapi32.dll', set()) | builtin.get('version.dll', set())
            if mod == 'msvcrt.dll' and name.startswith('_o_') and name[3:] in have:
                continue
            if name not in have:
                missing[(dll, name)].add(os.path.basename(f))
for (dll, name), users in sorted(missing.items()):
    print(f'{dll}!{name}  ({", ".join(sorted(users)[:3])}{"..." if len(users) > 3 else ""})')
print(f'{len(missing)} missing imports', file=sys.stderr)
