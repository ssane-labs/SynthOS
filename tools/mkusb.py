#!/usr/bin/env python3
"""Makes the USB stick image: the boot area, the program bundle and a small data partition that grows to the whole
stick on the first start. usage: mkusb.py synthos.img synthos-usb.img [data-MB]"""
import hashlib, os, struct, sys

SYSTEM = 16384
src, dst = sys.argv[1], sys.argv[2]
data_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 64
with open(src, "rb") as f:
    mbr = bytearray(f.read(512))
    f.seek(SYSTEM * 512)
    head = f.read(16)
    bundle = struct.unpack_from("<I", head, 12)[0] if head[:8] == b"SYNTHBDL" else 0
    data_lba = SYSTEM + bundle
    total = data_lba + data_mb * 2048
    struct.pack_into("<II", mbr, 446 + 2 * 16 + 8, data_lba, total - data_lba)
    f.seek(0)
    digest = hashlib.sha256()
    with open(dst + ".tmp", "wb") as out:
        left = data_lba * 512
        first = True
        while left:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                break
            if first:
                chunk = bytes(mbr) + chunk[512:]
                first = False
            out.write(chunk)
            digest.update(chunk)
            left -= len(chunk)
        zero = bytes(1 << 20)
        for _ in range(data_mb):
            out.write(zero)
            digest.update(zero)
os.replace(dst + ".tmp", dst)
with open(dst + ".sha256", "w") as f:
    f.write(f"{digest.hexdigest()}  {os.path.basename(dst)}\n")
print(f"  USB     {dst} ({os.path.getsize(dst) >> 20} MB, sha256 {digest.hexdigest()[:16]}...)")
