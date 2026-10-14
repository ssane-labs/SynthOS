#!/usr/bin/env python3
"""Checks that built-in Win32 exports clean up the stack like Windows does.

usage: tools/w32audit.py build/serial/kernel.elf

Each export's `ret N` in the kernel is compared with the argument list in Wine's
.spec file for the same DLL (downloaded once into build/wine-specs). A stdcall
function that pops the wrong number of bytes corrupts the caller's stack.
Needs python3-capstone and network access on the first run.
"""
import sys, struct, re, os, subprocess, urllib.request, capstone
elf = sys.argv[1] if len(sys.argv) > 1 else 'build/serial/kernel.elf'
CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(elf))), 'wine-specs')
# never return, or tail-call a cdecl helper
KNOWN = {'DebugBreak', 'ExitProcess', 'ExitThread', 'FatalExit', 'GetACP'}
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
    return None
def u32(va): return struct.unpack('<I', data[off(va):off(va) + 4])[0]
def cstr(va):
    o = off(va); e = data.index(b'\0', o); return data[o:e].decode()
syms = {}
for line in subprocess.run(['nm', elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
cache = {}
def cleanup(addr, depth=0):
    if addr in cache: return cache[addr]
    o = off(addr)
    res = None
    if o is not None:
        for ins in md.disasm(data[o:o + 6000], addr):
            if ins.mnemonic == 'ret':
                res = int(ins.op_str, 16) if ins.op_str else 0
                break
            if ins.mnemonic == 'jmp' and ins.op_str.startswith('0x') and depth < 4:
                t = int(ins.op_str, 16)
                if not (addr <= t < addr + 6000):
                    res = cleanup(t, depth + 1); break
    cache[addr] = res
    return res
mods = {}
a = syms['__start_synth_win32']
while a < syms['__stop_synth_win32']:
    d = u32(a); a += 4
    name, exp, count = cstr(u32(d)), u32(d + 4), u32(d + 8)
    for i in range(count):
        mods.setdefault(name.lower(), {})[cstr(u32(exp + i * 8))] = u32(exp + i * 8 + 4)
size = {'int64': 8, 'double': 8, 'int128': 16, 'long': 4, 'ptr': 4, 'str': 4, 'wstr': 4, 'float': 4, 'segptr': 4, 'segstr': 4}
def spec(dll):
    base = dll.rsplit('.', 1)[0]
    os.makedirs(CACHE, exist_ok=True)
    path = f'{CACHE}/{base}.spec'
    if not os.path.exists(path):
        try:
            txt = urllib.request.urlopen(f'https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/{base}/{base}.spec', timeout=20).read().decode()
        except Exception:
            txt = ''
        open(path, 'w').write(txt)
    out = {}
    for line in open(path):
        m = re.match(r'\s*(?:@|\d+)\s+(stdcall|cdecl|varargs|thiscall)\s+((?:-\S+\s+)*)(\S+)\s*\(([^)]*)\)', line)
        if not m: continue
        flags = m.group(2)
        if '-arch=' in flags and not re.search(r'-arch=[^ ]*(win32|i386)', flags) : continue
        args = m.group(4).split()
        out[m.group(3)] = (m.group(1), sum(size.get(t, 4) for t in args))
    return out
bad = 0
for dll in sorted(mods):
    sp = spec(dll)
    for fn, addr in sorted(mods[dll].items()):
        if fn not in sp or fn in KNOWN: continue
        conv, want = sp[fn]
        got = cleanup(addr)
        if got is None: continue
        if conv == 'stdcall' and got != want or conv in ('cdecl', 'varargs') and got != 0:
            bad += 1
            print(f'{dll:22s} {fn:40s} wine={conv}:{want:3d} ours=ret {got}')
print('mismatches:', bad, file=sys.stderr)
sys.exit(1 if bad else 0)
