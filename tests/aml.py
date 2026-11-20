"""A small AML assembler: enough of ACPI Machine Language to write test tables by hand (tests/acpi.py, the fuzz
seeds). Each function returns the bytes of one term; ssdt() wraps terms into a table with its header and checksum."""
import struct


def pkglen(body_len):
    """the PkgLength of a package whose contents (after the PkgLength) are body_len bytes"""
    for extra in range(4):
        total = body_len + 1 + extra
        if extra == 0 and total < 0x40:
            return bytes([total])
        if extra and total < 1 << (4 + 8 * extra):
            out = [(extra << 6) | (total & 15)]
            for i in range(extra):
                out.append((total >> (4 + 8 * i)) & 0xff)
            return bytes(out)
    raise ValueError("package too long")


def pkg(op, body):
    return op + pkglen(len(body)) + body


def seg(s):
    s = s.upper()
    return (s + "____")[:4].encode()


def name(path):
    out = b""
    if path.startswith("\\"):
        out += b"\\"
        path = path[1:]
    while path.startswith("^"):
        out += b"^"
        path = path[1:]
    segs = [p for p in path.split(".") if p]
    if not segs:
        return out + b"\x00"
    if len(segs) == 1:
        return out + seg(segs[0])
    if len(segs) == 2:
        return out + b"\x2e" + seg(segs[0]) + seg(segs[1])
    return out + b"\x2f" + bytes([len(segs)]) + b"".join(seg(s) for s in segs)


def Int(v):
    if v == 0:
        return b"\x00"
    if v == 1:
        return b"\x01"
    if v < 0x100:
        return b"\x0a" + bytes([v])
    if v < 0x10000:
        return b"\x0b" + struct.pack("<H", v)
    if v < 0x100000000:
        return b"\x0c" + struct.pack("<I", v)
    return b"\x0e" + struct.pack("<Q", v)


Ones = b"\xff"
Zero = b"\x00"
One = b"\x01"
Null = b"\x00"  # an empty target


def Str(s):
    return b"\x0d" + s.encode() + b"\x00"


def EisaId(s):
    v = ((ord(s[0]) - 64) << 26) | ((ord(s[1]) - 64) << 21) | ((ord(s[2]) - 64) << 16) | int(s[3:], 16)
    return Int(int.from_bytes(v.to_bytes(4, "big"), "little"))


def Buffer(data, size=None):
    return pkg(b"\x11", Int(len(data) if size is None else size) + bytes(data))


def Package(*items):
    return pkg(b"\x12", bytes([len(items)]) + b"".join(items))


def Local(i):
    return bytes([0x60 + i])


def Arg(i):
    return bytes([0x68 + i])


def N(path):
    """a name as an operand or target"""
    return name(path)


def Call(path, *args):
    return name(path) + b"".join(args)


def Scope(path, *terms):
    return pkg(b"\x10", name(path) + b"".join(terms))


def Device(path, *terms):
    return pkg(b"\x5b\x82", name(path) + b"".join(terms))


def Method(path, argc, *terms, serialized=False):
    return pkg(b"\x14", name(path) + bytes([argc | (8 if serialized else 0)]) + b"".join(terms))


def Name(path, value):
    return b"\x08" + name(path) + value


def Mutex(path):
    return b"\x5b\x01" + name(path) + b"\x00"


def OperationRegion(path, space, offset, length):
    return b"\x5b\x80" + name(path) + bytes([space]) + Int(offset) + Int(length)


def _fields(fields):
    """[(name or None, bits)] with None for a gap"""
    out = b""
    for n, bits in fields:
        if n is None:
            out += b"\x00" + _bits(bits)
        else:
            out += seg(n) + _bits(bits)
    return out


def _bits(n):
    """a field length: the PkgLength encoding of the number itself"""
    if n < 0x40:
        return bytes([n])
    extra = 1 if n < 1 << 12 else 2 if n < 1 << 20 else 3
    out = [(extra << 6) | (n & 15)]
    for i in range(extra):
        out.append((n >> (4 + 8 * i)) & 0xff)
    return bytes(out)


def Field(region, flags, *fields):
    return pkg(b"\x5b\x81", name(region) + bytes([flags]) + _fields(fields))


def IndexField(index, data, flags, *fields):
    return pkg(b"\x5b\x86", name(index) + name(data) + bytes([flags]) + _fields(fields))


def Return(v=b"\x00"):
    return b"\xa4" + v


def Store(src, dst):
    return b"\x70" + src + dst


def If(pred, *terms):
    return pkg(b"\xa0", pred + b"".join(terms))


def Else(*terms):
    return pkg(b"\xa1", b"".join(terms))


def While(pred, *terms):
    return pkg(b"\xa2", pred + b"".join(terms))


Break = b"\xa5"


def _bin(op):
    return lambda a, b, target=Null: bytes([op]) + a + b + target


Add, Subtract, Multiply = _bin(0x72), _bin(0x74), _bin(0x77)
ShiftLeft, ShiftRight, And, Or, XOr, Mod = _bin(0x79), _bin(0x7A), _bin(0x7B), _bin(0x7D), _bin(0x7F), _bin(0x85)
Concat = _bin(0x73)


def Divide(a, b, rem=Null, quot=Null):
    return b"\x78" + a + b + rem + quot


def Increment(t):
    return b"\x75" + t


def Not(a, target=Null):
    return b"\x80" + a + target


def LNot(a):
    return b"\x92" + a


def LEqual(a, b):
    return b"\x93" + a + b


def LGreater(a, b):
    return b"\x94" + a + b


def LLess(a, b):
    return b"\x95" + a + b


def LNotEqual(a, b):
    return LNot(LEqual(a, b))


def LAnd(a, b):
    return b"\x90" + a + b


def Index(src, i, target=Null):
    return b"\x88" + src + i + target


def DerefOf(r):
    return b"\x83" + r


def RefOf(t):
    return b"\x71" + t


def CondRefOf(t, target=Null):
    return b"\x5b\x12" + t + target


def SizeOf(t):
    return b"\x87" + t


def ObjectType(t):
    return b"\x8e" + t


def Notify(t, v):
    return b"\x86" + t + v


def CreateDWordField(buf, index, path):
    return b"\x8a" + buf + index + name(path)


def CreateByteField(buf, index, path):
    return b"\x8c" + buf + index + name(path)


def CreateField(buf, bit, bits, path):
    return b"\x5b\x13" + buf + bit + bits + name(path)


def ToHexString(a, target=Null):
    return b"\x98" + a + target


def ToInteger(a, target=Null):
    return b"\x99" + a + target


def Mid(a, i, n, target=Null):
    return b"\x9e" + a + i + n + target


def Match(p, op1, v1, op2, v2, start):
    return b"\x89" + p + bytes([op1]) + v1 + bytes([op2]) + v2 + start


def Sleep(ms):
    return b"\x5b\x22" + ms


def check(n, got, want):
    """If (got != want) Return (n): a numbered check inside a test method"""
    return If(LNotEqual(got, want), Return(Int(n)))


def table(sig, terms, revision=2, oem_table=b"SYNTHTST"):
    body = b"".join(terms)
    head = sig + struct.pack("<IB", 36 + len(body), revision) + b"\x00" + b"SYNTH " + oem_table + \
        struct.pack("<I", 1) + b"SYNT" + struct.pack("<I", 1)
    t = bytearray(head + body)
    t[9] = (-sum(t)) & 0xff
    return bytes(t)


def ssdt(*terms):
    return table(b"SSDT", terms)
