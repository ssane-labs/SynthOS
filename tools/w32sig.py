#!/usr/bin/env python3
"""Signatures of the built-in Windows functions for the 64-bit kernel's call gate.

usage: w32sig.py SOURCE.c OUT.h CLANG [flags...]

A 32-bit program pushes arguments the way the 32-bit kernel's functions expect them (stdcall or cdecl, i386); the
64-bit kernel's functions take them in registers (System V). The source is compiled to LLVM IR twice, as i386 and
as x86-64, and for every function a 32-bit program can reach (stdcall functions and the targets of export tables)
the header gets a gate entry: where each 64-bit argument comes from on the 32-bit stack, how the result goes back
and how many bytes the callee pops. The header is included at the end of the source, so static functions count."""

import re
import subprocess
import sys

KINDS = {'ZX32': 1, 'I64': 2, 'F32': 3, 'F64': 4, 'SX8': 5, 'ZX8': 6, 'SX16': 7, 'ZX16': 8, 'MEM': 9, 'PIECE': 10,
         'PIECE_F64': 11, 'F80': 12, 'VARARGS': 13}
RETS = {'void': 0, 'i32': 1, 'i64': 2, 'float': 3, 'double': 4, 'x86_fp80': 5, 'sret': 6, 'i8': 1, 'i16': 1, 'i1': 1,
        'ptr': 1, 'pair': 7}


def ir(source, target, flags):
    cmd = [flags[0], '--target=' + target, '-S', '-emit-llvm', '-fno-discard-value-names', '-O1', '-w', '-o', '-'] + \
        flags[1:] + [source]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '({[<':
            depth += 1
        elif ch in ')}]>':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def struct_sizes(text, ptr):
    """byte sizes of the named struct types (i386 or x86-64 layout)"""
    defs = {}
    for m in re.finditer(r'^(%[\w.$"]+) = type (<?\{.*\}>?|\[.*\]|opaque)$', text, re.M):
        defs[m.group(1)] = m.group(2)
    cache = {}

    def size_align(t):
        t = t.strip()
        if t in cache:
            return cache[t]
        if t in ('i1', 'i8'):
            r = (1, 1)
        elif t == 'i16':
            r = (2, 2)
        elif t in ('i32', 'float'):
            r = (4, 4)
        elif t in ('i64', 'double'):
            r = (8, 8 if ptr == 8 else 4)
        elif t == 'x86_fp80':
            r = (16, 16) if ptr == 8 else (12, 4)
        elif t.startswith('ptr'):
            r = (4, 4) if 'addrspace(270)' in t or ptr == 4 else (8, 8)
        elif t.startswith('['):
            m = re.match(r'\[(\d+) x (.*)\]$', t)
            s, a = size_align(m.group(2))
            r = (s * int(m.group(1)), a)
        elif t.startswith('<{'):
            r = (sum(size_align(f)[0] for f in split_top(t[2:-2])), 1)
        elif t.startswith('{'):
            off, al = 0, 1
            for f in split_top(t[1:-1]):
                s, a = size_align(f)
                off = (off + a - 1) // a * a + s
                al = max(al, a)
            r = ((off + al - 1) // al * al, al)
        elif t.startswith('%'):
            body = defs.get(t)
            r = size_align(body) if body and body != 'opaque' else (0, 1)
        else:
            raise ValueError('type ' + t)
        cache[t] = r
        return r
    return size_align


ATTRS = re.compile(r'\b(noundef|nonnull|readonly|writeonly|nocapture|returned|noalias|readnone|immarg|inreg|nofree|dead_on_unwind|writable|'
                   r'align \d+|dereferenceable(_or_null)?\(\d+\)|captures\([^)]*\)|initializes\([^)]*\)|range\([^)]*\))\s*')


def functions(text):
    out = {}
    for m in re.finditer(r'^define (.*?)@("?[\w.$]+"?)\((.*)\)([^{]*)\{', text, re.M):
        pre, name, params = m.group(1), m.group(2).strip('"'), m.group(3)
        ret = ATTRS.sub('', re.sub(r'\b(dso_local|internal|private|hidden|noundef|zeroext|signext|'
                                    r'x86_stdcallcc|x86_fastcallcc|x86_thiscallcc|range\([^)]*\))\s*', '', pre)).strip()
        out[name] = {'stdcall': 'x86_stdcallcc' in pre, 'other_cc': 'fastcallcc' in pre or 'thiscallcc' in pre,
                     'ret': ret, 'params': [ATTRS.sub('', p).strip() for p in split_top(params)]}
    # functions defined elsewhere (the kernel's string functions in msvcrt's tables) by their prototypes
    for m in re.finditer(r'^declare (.*?)@("?[\w.$]+"?)\((.*)\)', text, re.M):
        pre, name, params = m.group(1), m.group(2).strip('"'), m.group(3)
        if name in out or name.startswith('llvm.'):
            continue
        ret = ATTRS.sub('', re.sub(r'\b(dso_local|internal|private|hidden|noundef|zeroext|signext|'
                                    r'x86_stdcallcc|x86_fastcallcc|x86_thiscallcc|range\([^)]*\))\s*', '', pre)).strip()
        out[name] = {'stdcall': False, 'other_cc': 'fastcallcc' in pre or 'thiscallcc' in pre,
                     'ret': ret, 'params': [ATTRS.sub('', p).strip() for p in split_top(params)], 'declared': True}
    return out


def guest_param(p, size32):
    """(kind of value, size on the 32-bit stack) of one i386 parameter"""
    if p == '...':
        return ('...', 0, None)
    m = re.match(r'ptr (sret|byval)\((.*?)\)', p)
    if m:
        return (m.group(1), (size32(m.group(2))[0] + 3) // 4 * 4, m.group(2))
    t = p.split()[0] if not p.startswith('ptr') else 'ptr'
    if p.startswith('signext') or p.startswith('zeroext'):
        t = p.split()[1]
    size = {'i1': 4, 'i8': 4, 'i16': 4, 'i32': 4, 'ptr': 4, 'float': 4, 'i64': 8, 'double': 8, 'x86_fp80': 12}.get(t)
    if size is None:
        raise ValueError('guest parameter ' + p)
    return (t, size, None)


def host_param(p):
    m = re.match(r'ptr (sret|byval)\((.*?)\)', p)
    if m:
        return (m.group(1), m.group(2))
    t = p.rsplit(' %', 1)[0].strip() if ' %' in p else p
    ext = 'signext' if 'signext' in t.split() else 'zeroext' if 'zeroext' in t.split() else ''
    t = ' '.join(w for w in t.split() if w not in ('signext', 'zeroext'))
    return (ext + ' ' + t).strip(), None


def plan(name, g, h, size32, size64):
    """the gate entry: (pop bytes, return kind, [(kind, guest offset, extra)])"""
    gps = [guest_param(p, size32) for p in g['params']]
    hps = [host_param(p) for p in h['params']]
    args, gi, hi, off = [], 0, 0, 0
    ret = None
    if gps and gps[0][0] == 'sret':
        ret = 'sret'
        if hps and hps[0][0] == 'sret':
            args.append(('ZX32', 0, 0))
            hi = 1
        else:
            ret = 'pair'
        off, gi = 4, 1
    while gi < len(gps):
        kind, size, struct = gps[gi]
        if kind == '...':
            args.append(('VARARGS', off, 0))
            break
        if kind == 'byval':
            if hi < len(hps) and hps[hi][0] == 'byval':
                args.append(('MEM', off, size))
                hi += 1
            else:
                piece = 0
                while piece < size and hi < len(hps):
                    t = 'ptr' if 'ptr' in hps[hi][0] else hps[hi][0].split()[-1]
                    if t in ('i64', 'ptr'):
                        args.append(('PIECE', off + piece, min(8, size - piece)))
                        piece += 8
                    elif t == 'i32':
                        args.append(('PIECE', off + piece, 4))
                        piece += 4
                    elif t == 'double':
                        args.append(('PIECE_F64', off + piece, 8))
                        piece += 8
                    else:
                        raise ValueError('%s: struct piece %s' % (name, t))
                    hi += 1
        else:
            if hi >= len(hps):
                raise ValueError('%s: fewer 64-bit parameters' % name)
            ht = hps[hi][0]
            t = 'ptr' if ht.split()[-1].startswith('addrspace') or ht.endswith('ptr') else ht.split()[-1]
            if t in ('float',):
                args.append(('F32', off, 0))
            elif t == 'double':
                args.append(('F64', off, 0))
            elif t == 'x86_fp80':
                args.append(('F80', off, 0))
            elif kind == 'i64' or (t == 'i64' and size == 8):
                args.append(('I64', off, 0))
            elif ht.startswith('signext') and t == 'i8':
                args.append(('SX8', off, 0))
            elif ht.startswith('signext') and t == 'i16':
                args.append(('SX16', off, 0))
            elif t == 'i8' or t == 'i1':
                args.append(('ZX8', off, 0))
            elif t == 'i16':
                args.append(('ZX16', off, 0))
            elif t in ('i32', 'i64') or t.startswith('ptr'):
                args.append(('ZX32', off, 0))
            else:
                raise ValueError('%s: parameter %s' % (name, ht))
            hi += 1
        off += size
        gi += 1
    if args and args[-1][0] == 'VARARGS':
        hi = len(hps)
    if hi != len(hps):
        raise ValueError('%s: %d extra 64-bit parameters' % (name, len(hps) - hi))
    if ret is None:
        r = g['ret']
        hr = h['ret']
        if r == 'void':
            ret = 'void'
        elif r in ('i64',) and hr in ('i64',):
            ret = 'i64'
        elif r in RETS:
            ret = r if r not in ('i8', 'i16', 'i1', 'ptr') else 'i32'
            if hr.startswith('{') and r == 'i64':
                ret = 'pair'
        else:
            raise ValueError('%s: return %s / %s' % (name, r, hr))
    return (off if g['stdcall'] else 0, ret, args)


def export_targets(text):
    names = set()
    for m in re.finditer(r'^@[\w.$"]+ = .*?\[\d+ x %struct\.w32_export\] \[(.*)\]', text, re.M):
        names.update(re.findall(r'ptr @("?[\w.$]+"?)', m.group(1)))
    return {n.strip('"') for n in names}


def main():
    source, out, flags = sys.argv[1], sys.argv[2], sys.argv[3:]
    t32 = ir(source, 'i686-unknown-none-elf', flags)
    t64 = ir(source, 'x86_64-unknown-none-elf', flags)
    f32, f64 = functions(t32), functions(t64)
    size32, size64 = struct_sizes(t32, 4), struct_sizes(t64, 8)
    wanted = {n for n, f in f32.items() if f['stdcall'] and 'declared' not in f} | export_targets(t32) | \
        set(re.findall(r'^@w32_gate_ref_\w+ = .*?ptr @("?[\w.$]+"?)', t64, re.M))
    lines, errors = [], []
    for name in sorted(wanted):
        if name not in f32 or name not in f64 or f32[name]['other_cc']:
            continue
        try:
            pop, ret, args = plan(name, f32[name], f64[name], size32, size64)
        except ValueError as e:
            errors.append(str(e))
            continue
        code = ''.join('\\x%02x\\x%02x\\x%02x' % (KINDS[k], o // 4, (x + 3) // 4) for k, o, x in args)
        lines.append('    {(const void *)%s, %d, %d, %d, "%s"},' % (name, pop, RETS[ret], len(args), code))
    if errors:
        sys.stderr.write('\n'.join('w32sig: %s: %s' % (source, e) for e in errors) + '\n')
        sys.exit(1)
    with open(out, 'w') as f:
        f.write('/* generated by tools/w32sig.py from %s */\n' % source)
        if lines:
            f.write('static const struct w32_sig w32_sigs_here[] __attribute__((section("w32sigs"), used, aligned(8))) = {\n')
            f.write('\n'.join(lines) + '\n};\n')


if __name__ == '__main__':
    main()
