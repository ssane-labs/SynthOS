#!/usr/bin/env python3
"""Makes a disk like a PC with Windows after tools/windows/install.ps1 ran on it: GPT, the Windows EFI partition
(FAT32) with \\EFI\\SynthOS and \\SYNTHOS, a "Windows" partition made smaller, and the SynthOS partition behind it
written the way the script writes it. The UEFI Shell's startup.nsh stands in for the boot entry bcdedit adds.
usage: winsim.py synthos.img disk.img"""
import os, struct, subprocess, sys, tempfile, uuid, zlib

src, dst = sys.argv[1], sys.argv[2]
DISK = 4 * 1024 * 1024 * 2           # 4 GiB in sectors
ESP_START, ESP_SECTORS = 2048, 204800
WIN_START = ESP_START + ESP_SECTORS
SYNTH_SECTORS = 1024 * 1024 * 2      # what "-SizeGB 1" gives
WIN_END = DISK - 2048 - SYNTH_SECTORS
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
DATA_TYPE = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")
SYNTH_TYPE = uuid.UUID("53594e54-484f-4f53-8053-796e74684f53")


def gpt(f, parts):
    entries = bytearray(128 * 128)
    for i, (kind, first, last, name) in enumerate(parts):
        struct.pack_into("<16s16sQQQ72s", entries, i * 128, kind.bytes_le, uuid.uuid4().bytes_le, first, last, 0,
                         name.encode("utf-16-le"))
    crc = zlib.crc32(entries)
    disk_guid = uuid.uuid4().bytes_le
    for my, alt, table in ((1, DISK - 1, 2), (DISK - 1, 1, DISK - 33)):
        h = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0, my, alt, 34, DISK - 34,
                         disk_guid, table, 128, 128, crc)
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        f.seek(my * 512)
        f.write(bytes(h).ljust(512, b"\0"))
        f.seek(table * 512)
        f.write(entries)
    mbr = bytearray(512)
    struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, DISK - 1)
    mbr[510:512] = b"\x55\xaa"
    f.seek(0)
    f.write(mbr)


with open(src, "rb") as img:
    sector0 = bytearray(img.read(512))
    assert sector0[416:420] == b"SYID"
    system = 32768 if struct.unpack_from("<I", sector0, 428)[0] & 2 else 16384
    img.seek(system * 512)
    head = img.read(16)
    bundle = struct.unpack_from("<I", head, 12)[0] if head[:8] == b"SYNTHBDL" else 0
    img.seek(0)
    boot_area = img.read(8192 * 512)
    img.seek(system * 512)
    bundle_data = img.read(bundle * 512)

work = tempfile.mkdtemp()
esp_src = f"{src}@@{8192 * 512}"


def from_esp(path, name):
    out = os.path.join(work, name)
    r = subprocess.run(["mcopy", "-o", "-i", esp_src, "::" + path, out], capture_output=True)
    return out if r.returncode == 0 else None


shim = from_esp("/EFI/BOOT/grubx64.efi", "grubx64.efi")
files = {"/SYNTHOS/KERNEL.BIN": from_esp("/SYNTHOS/KERNEL.BIN", "KERNEL.BIN")}
if shim:
    for n in ("shimx64.efi", "mmx64.efi"):
        files["/EFI/SynthOS/" + n] = from_esp("/EFI/BOOT/" + ("BOOTX64.EFI" if n == "shimx64.efi" else n), n)
    files["/EFI/SynthOS/grubx64.efi"] = shim
    loader = r"\EFI\SynthOS\shimx64.efi"
else:
    files["/EFI/SynthOS/synthos.efi"] = from_esp("/EFI/BOOT/BOOTX64.EFI", "synthos.efi")
    loader = r"\EFI\SynthOS\synthos.efi"

lba = WIN_END + 2048 - (WIN_END % 2048) if WIN_END % 2048 else WIN_END
sectors = DISK - 34 - lba
sectors -= sectors % 2048
record = bytearray(sector0)
ident = os.urandom(8)
record[420:428] = ident
struct.pack_into("<I", record, 428, struct.unpack_from("<I", record, 428)[0] | 1)
record[446:510] = bytes(64)
cfg = (f"# SynthOS installation: the loader reads this file\nsystem={lba}\nsectors={sectors}\n"
       f"id={ident.hex()}\nregister=1\ntimeout=3\n")
open(os.path.join(work, "SYSTEM.CFG"), "w").write(cfg)
files["/SYNTHOS/SYSTEM.CFG"] = os.path.join(work, "SYSTEM.CFG")
open(os.path.join(work, "startup.nsh"), "w").write(f"fs0:{loader}\r\n")
files["/startup.nsh"] = os.path.join(work, "startup.nsh")

esp = os.path.join(work, "esp.img")
subprocess.run(["mkfs.fat", "-F", "32", "-n", "SYSTEM", "-C", esp, str(ESP_SECTORS // 2)], check=True,
               stdout=subprocess.DEVNULL)
subprocess.run(["mmd", "-i", esp, "::/EFI", "::/EFI/Microsoft", "::/EFI/SynthOS", "::/SYNTHOS"], check=True)
for path, local in files.items():
    subprocess.run(["mcopy", "-i", esp, local, "::" + path], check=True)

with open(dst, "wb") as f:
    f.truncate(DISK * 512)
    gpt(f, [(ESP_TYPE, ESP_START, WIN_START - 1, "EFI system partition"),
            (DATA_TYPE, WIN_START, WIN_END - 1, "Basic data partition"),
            (SYNTH_TYPE, lba, lba + sectors - 1, "")])
    f.seek(ESP_START * 512)
    f.write(open(esp, "rb").read())
    f.seek(lba * 512)
    f.write(boot_area)
    f.seek((lba + system) * 512)
    f.write(bundle_data)
    f.seek((lba + system + bundle) * 512)
    f.write(bytes(16 * 512))
    f.seek(lba * 512)
    f.write(record)
print(f"system at sector {lba}")
