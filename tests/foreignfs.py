#!/usr/bin/env python3
"""Writes a test disk with an MBR and two partitions: exFAT (p1) and NTFS (p2), each with the files below.
Only what SynthOS's read-only drivers read is filled in (no upcase table, no $LogFile...), so these volumes are
for the tests, not for Windows. usage: foreignfs.py OUT.img"""
import struct
import sys

SECTOR = 512
BIG = bytes((i * 7 + i // 251) & 0xff for i in range(70000))   # spans several clusters
FILES = {
    "readme.txt": b"hello from exfat\n",
    "Docs/Report Ünïcode.txt": b"nested file\n",
    "big.bin": BIG,
}


def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def u64(v): return struct.pack("<Q", v)


# ---------------- exFAT ----------------
def exfat(size):
    spc_shift = 3                         # 4 KB clusters
    cb = SECTOR << spc_shift
    fat_off = 128                         # sectors
    heap_off = 256
    clusters = (size // SECTOR - heap_off) >> spc_shift
    img = bytearray(size)
    fat = {}
    nxt = [2]

    def alloc(n, chain_gap=False):
        """n clusters; chain_gap leaves a hole between them so the file needs its FAT chain"""
        out = []
        for _ in range(n):
            out.append(nxt[0])
            nxt[0] += 2 if chain_gap else 1
        for a, b in zip(out, out[1:]):
            fat[a] = b
        if out:
            fat[out[-1]] = 0xffffffff
        return out

    def put(cl, data):
        for i, c in enumerate(cl):
            chunk = data[i * cb:(i + 1) * cb]
            o = (heap_off << 9) + (c - 2) * cb
            img[o:o + len(chunk)] = chunk

    def entry_set(name, attr, first, length, nofat):
        name16 = name.encode("utf-16-le")
        chars = len(name16) // 2
        n_names = (chars + 14) // 15
        f = bytearray(32)
        f[0] = 0x85
        f[1] = 1 + n_names
        f[4:6] = u16(attr)
        f[12:16] = u32((45 << 25) | (6 << 21) | (15 << 16) | (12 << 11))   # 2025-06-15 12:00
        s = bytearray(32)
        s[0] = 0xc0
        s[1] = 1 | (2 if nofat else 0)
        s[3] = chars
        s[8:16] = u64(length)
        s[20:24] = u32(first)
        s[24:32] = u64(length)
        out = f + s
        for k in range(n_names):
            e = bytearray(32)
            e[0] = 0xc1
            part = name16[k * 30:(k + 1) * 30]
            e[2:2 + len(part)] = part
            out += e
        return out

    bitmap_cl = alloc(1)
    root_cl = alloc(1)
    docs_cl = alloc(1)
    readme_cl = alloc(1)
    report_cl = alloc(1)
    big_n = (len(BIG) + cb - 1) // cb
    big_cl = alloc(big_n, chain_gap=True)
    # the directory "Docs"
    docs = entry_set("Report Ünïcode.txt", 0x20, report_cl[0], len(FILES["Docs/Report Ünïcode.txt"]), True)
    put(docs_cl, bytes(docs))
    put(readme_cl, FILES["readme.txt"])
    put(report_cl, FILES["Docs/Report Ünïcode.txt"])
    put(big_cl, BIG)
    used = nxt[0]
    bitmap = bytearray((clusters + 7) // 8)
    for c in range(2, used):
        bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    put(bitmap_cl, bytes(bitmap))
    root = bytearray()
    label = "EXTEST".encode("utf-16-le")
    root += bytes([0x83, 6]) + label + bytes(32 - 2 - len(label))
    root += bytes([0x81, 0]) + bytes(18) + u32(bitmap_cl[0]) + u64(len(bitmap))
    root += entry_set("readme.txt", 0x20, readme_cl[0], len(FILES["readme.txt"]), True)
    root += entry_set("Docs", 0x10, docs_cl[0], cb, True)
    root += entry_set("big.bin", 0x20, big_cl[0], len(BIG), False)
    put(root_cl, bytes(root))
    # FAT: only the chained file needs entries, the rest are contiguous
    for c, v in fat.items():
        o = fat_off * SECTOR + c * 4
        img[o:o + 4] = u32(v)
    b = bytearray(SECTOR)
    b[0:3] = b"\xeb\x76\x90"
    b[3:11] = b"EXFAT   "
    b[72:80] = u64(size // SECTOR)
    b[80:84] = u32(fat_off)
    b[84:88] = u32(heap_off - fat_off)
    b[88:92] = u32(heap_off)
    b[92:96] = u32(clusters)
    b[96:100] = u32(root_cl[0])
    b[108] = 9
    b[109] = spc_shift
    b[110] = 1
    b[510:512] = b"\x55\xaa"
    img[0:SECTOR] = b
    return bytes(img)


# ---------------- NTFS ----------------
REC = 1024
INDX = 4096
CB = 4096


def fixed(data, size, magic):
    """a record with its update sequence: the last two bytes of each sector move into the array"""
    d = bytearray(data.ljust(size, b"\0"))
    d[0:4] = magic
    at = 0x28 if magic == b"INDX" else 0x30
    count = size // SECTOR + 1
    d[4:6] = u16(at)
    d[6:8] = u16(count)
    usn = b"\x01\x00"
    d[at:at + 2] = usn
    for i in range(1, count):
        end = i * SECTOR - 2
        d[at + i * 2:at + i * 2 + 2] = d[end:end + 2]
        d[end:end + 2] = usn
    return bytes(d)


def attr_resident(atype, value, name=""):
    nm = name.encode("utf-16-le")
    off = 24 + len(nm)
    off = (off + 7) & ~7
    length = (off + len(value) + 7) & ~7
    a = bytearray(length)
    a[0:4] = u32(atype)
    a[4:8] = u32(length)
    a[8] = 0
    a[9] = len(nm) // 2
    a[10:12] = u16(24)
    a[24:24 + len(nm)] = nm
    a[16:20] = u32(len(value))
    a[20:22] = u16(off)
    a[off:off + len(value)] = value
    return bytes(a)


def runlist(runs):
    out = bytearray()
    prev = 0
    for lcn, n in runs:
        ln = n.to_bytes(8, "little").rstrip(b"\0") or b"\0"
        d = lcn - prev
        prev = lcn
        dl = d.to_bytes(8, "little", signed=True)
        k = 8
        while k > 1 and ((dl[k - 1] == 0 and dl[k - 2] < 0x80) or (dl[k - 1] == 0xff and dl[k - 2] >= 0x80)):
            k -= 1
        out += bytes([len(ln) | (k << 4)]) + ln + dl[:k]
    return bytes(out + b"\0")


def attr_nonresident(atype, runs, size, name=""):
    nm = name.encode("utf-16-le")
    rl = runlist(runs)
    off = (64 + len(nm) + 7) & ~7
    length = (off + len(rl) + 7) & ~7
    total = sum(n for _, n in runs)
    a = bytearray(length)
    a[0:4] = u32(atype)
    a[4:8] = u32(length)
    a[8] = 1
    a[9] = len(nm) // 2
    a[10:12] = u16(64)
    a[64:64 + len(nm)] = nm
    a[16:24] = u64(0)
    a[24:32] = u64(total - 1)
    a[32:34] = u16(off)
    a[40:48] = u64(total * CB)
    a[48:56] = u64(size)
    a[56:64] = u64(size)
    a[off:off + len(rl)] = rl
    return bytes(a)


def record(attrs, directory=False):
    body = bytearray(0x38)
    body[16:18] = u16(1)
    body[20:22] = u16(0x38)
    body[22:24] = u16(1 | (2 if directory else 0))
    for a in attrs:
        body += a
    body += b"\xff\xff\xff\xff"
    body[24:28] = u32(len(body))
    body[28:32] = u32(REC)
    return fixed(bytes(body), REC, b"FILE")


def file_name(parent, name, size, directory):
    nm = name.encode("utf-16-le")
    v = bytearray(66)
    v[0:8] = u64(parent | (1 << 48))
    ft = (1750000000 + 11644473600) * 10000000
    for o in (8, 16, 24, 32):
        v[o:o + 8] = u64(ft)
    v[40:48] = u64(size)
    v[48:56] = u64(size)
    v[56:60] = u32(0x10000000 if directory else 0x20)
    v[64] = len(nm) // 2
    v[65] = 1
    return bytes(v) + nm


def index_entries(children):
    out = bytearray()
    for ref, name, size, directory in sorted(children, key=lambda c: c[1].upper()):
        key = file_name(0, name, size, directory)
        ln = (16 + len(key) + 7) & ~7
        e = bytearray(ln)
        e[0:8] = u64(ref | (1 << 48))
        e[8:10] = u16(ln)
        e[10:12] = u16(len(key))
        e[16:16 + len(key)] = key
        out += e
    end = bytearray(16)
    end[8:10] = u16(16)
    end[12:14] = u16(2)
    return bytes(out + end)


def ntfs(size):
    img = bytearray(size)
    clusters = size // CB
    mft_lcn = 4
    nrec = 48
    mft_clusters = nrec * REC // CB
    nxt = [mft_lcn + mft_clusters]

    def alloc(n, gap=False):
        runs = []
        for _ in range(n):
            lcn = nxt[0]
            nxt[0] += 2 if gap else 1
            if runs and runs[-1][0] + runs[-1][1] == lcn:
                runs[-1] = (runs[-1][0], runs[-1][1] + 1)
            else:
                runs.append((lcn, 1))
        return runs

    def write_runs(runs, data):
        pos = 0
        for lcn, n in runs:
            chunk = data[pos:pos + n * CB]
            img[lcn * CB:lcn * CB + len(chunk)] = chunk
            pos += n * CB

    recs = {}
    recs[0] = record([attr_nonresident(0x80, [(mft_lcn, mft_clusters)], nrec * REC)])
    recs[3] = record([attr_resident(0x60, "NTTEST".encode("utf-16-le"))])
    # files
    recs[24] = record([attr_resident(0x30, file_name(5, "readme.txt", 17, False)),
                       attr_resident(0x80, b"hello from ntfs!\n")])
    big_runs = alloc((len(BIG) + CB - 1) // CB, gap=True)
    write_runs(big_runs, BIG)
    recs[25] = record([attr_resident(0x30, file_name(5, "big.bin", len(BIG), False)),
                       attr_nonresident(0x80, big_runs, len(BIG))])
    recs[27] = record([attr_resident(0x30, file_name(26, "Report Ünïcode.txt", 12, False)),
                       attr_resident(0x80, b"nested file\n")])
    # "Docs" keeps its entries in an index block (INDEX_ALLOCATION + BITMAP), the root in INDEX_ROOT
    entries = index_entries([(27, "Report Ünïcode.txt", 12, False)])
    hdr = bytearray(16)
    hdr[0:4] = u32(0x28)   # past the update sequence array at 0x28
    hdr[4:8] = u32(0x28 + len(entries))
    hdr[8:12] = u32(INDX - 24)
    blk = bytearray(24) + hdr + bytes(0x28 - 16) + entries
    blk[16:24] = u64(0)
    blk = fixed(bytes(blk), INDX, b"INDX")
    docs_runs = alloc(1)
    write_runs(docs_runs, blk)
    sub_e = bytearray(24)
    sub_e[8:10] = u16(24)
    sub_e[12:14] = u16(3)
    ihdr = bytearray(16)
    ihdr[0:4] = u32(16)
    ihdr[4:8] = u32(16 + len(sub_e))
    ihdr[8:12] = u32(16 + len(sub_e))
    ihdr[12] = 1
    iroot = u32(0x30) + u32(1) + u32(INDX) + bytes([1, 0, 0, 0]) + bytes(ihdr) + bytes(sub_e)
    recs[26] = record([attr_resident(0x30, file_name(5, "Docs", 0, True)),
                       attr_resident(0x90, iroot, "$I30"),
                       attr_nonresident(0xa0, docs_runs, INDX, "$I30"),
                       attr_resident(0xb0, b"\x01\0\0\0\0\0\0\0", "$I30")], directory=True)
    root_entries = index_entries([(24, "readme.txt", 17, False), (25, "big.bin", len(BIG), False),
                                  (26, "Docs", 0, True), (5, ".", 0, True)])
    rhdr = bytearray(16)
    rhdr[0:4] = u32(16)
    rhdr[4:8] = u32(16 + len(root_entries))
    rhdr[8:12] = u32(16 + len(root_entries))
    rroot = u32(0x30) + u32(1) + u32(INDX) + bytes([1, 0, 0, 0]) + bytes(rhdr) + root_entries
    recs[5] = record([attr_resident(0x30, file_name(5, ".", 0, True)), attr_resident(0x90, rroot, "$I30")],
                     directory=True)
    # $Bitmap
    used = nxt[0]
    bm = bytearray((clusters + 7) // 8)
    for c in range(used):
        bm[c // 8] |= 1 << (c % 8)
    bm_runs = alloc((len(bm) + CB - 1) // CB)
    for c in range(used, nxt[0]):
        bm[c // 8] |= 1 << (c % 8)
    write_runs(bm_runs, bytes(bm))
    recs[6] = record([attr_nonresident(0x80, bm_runs, len(bm))])
    for n, r in recs.items():
        o = mft_lcn * CB + n * REC
        img[o:o + REC] = r
    b = bytearray(SECTOR)
    b[0:3] = b"\xeb\x52\x90"
    b[3:11] = b"NTFS    "
    b[11:13] = u16(512)
    b[13] = CB // SECTOR
    b[21] = 0xf8
    b[40:48] = u64(size // SECTOR - 1)
    b[48:56] = u64(mft_lcn)
    b[56:64] = u64(mft_lcn)
    b[64] = 0xf6   # 2^10 = 1024-byte records
    b[68] = 1      # one cluster per index block
    b[510:512] = b"\x55\xaa"
    img[0:SECTOR] = b
    return bytes(img)


def main():
    part = 8 << 20
    ex = exfat(part)
    nt = ntfs(part)
    mbr = bytearray(SECTOR)
    start = 2048
    for i, (kind, n) in enumerate(((0x07, len(ex)), (0x07, len(nt)))):
        e = 446 + i * 16
        mbr[e + 4] = kind
        mbr[e + 8:e + 12] = u32(start)
        mbr[e + 12:e + 16] = u32(n // SECTOR)
        start += n // SECTOR
    mbr[510:512] = b"\x55\xaa"
    with open(sys.argv[1], "wb") as f:
        f.write(mbr + bytes((2048 - 1) * SECTOR) + ex + nt)


if __name__ == "__main__":
    main()
