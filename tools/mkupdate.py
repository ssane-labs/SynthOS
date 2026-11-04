#!/usr/bin/env python3
"""Packs a SynthOS system update: the boot sectors (stage1 code, stage2, kernel with its ramdisk) and the files of
the EFI partition, so an installed system can update itself without a reinstall ('update' inside SynthOS).

usage: tools/mkupdate.py OUT.sup ARCH stage1.bin stage2.bin kernel.bin esp.img
Writes OUT.sup and OUT.txt (the manifest 'update check' reads). The ESP files come out of esp.img with mtools.
Format: "SYNUPD01", u32 arch, u32 count, char build[48], then count entries of name[56] + u32 size + u32 same,
then the data of each entry whose `same` is 0 (otherwise the entry is a copy of entry same-1), then the SHA-256 of
everything before it.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time


def esp_files(image):
    out = tempfile.mkdtemp()
    subprocess.run(["mcopy", "-s", "-n", "-i", image, "::/*", out], check=True, stdout=subprocess.DEVNULL)
    files = []
    for root, _, names in os.walk(out):
        for n in sorted(names):
            p = os.path.join(root, n)
            rel = os.path.relpath(p, out).replace(os.sep, "/")
            if rel.upper() == "SYNTHOS/SYSTEM.CFG":
                continue
            with open(p, "rb") as f:
                files.append(("esp/" + rel, f.read()))
    return files


def build_id():
    try:
        sha = subprocess.run(["git", "rev-parse", "--short=10", "HEAD"], capture_output=True, text=True,
                             check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        sha = "local"
    return "%s %s" % (sha, time.strftime("%Y-%m-%d", time.gmtime()))


def main():
    out, arch, stage1, stage2, kernel, esp = sys.argv[1:7]
    entries = []
    for name, path in (("boot/stage1", stage1), ("boot/stage2", stage2), ("boot/kernel", kernel)):
        with open(path, "rb") as f:
            entries.append((name, f.read()))
    entries += esp_files(esp)
    build = os.environ.get("BUILD_ID") or build_id()
    blob = bytearray(b"SYNUPD01" + struct.pack("<II", int(arch), len(entries)) + build.encode()[:47].ljust(48, b"\0"))
    same = [next((j + 1 for j in range(i) if entries[j][1] == entries[i][1]), 0) for i in range(len(entries))]
    for (name, data), ref in zip(entries, same):
        blob += name.encode()[:55].ljust(56, b"\0") + struct.pack("<II", len(data), ref)
    for (_, data), ref in zip(entries, same):
        if not ref:
            blob += data
    blob += hashlib.sha256(blob).digest()
    with open(out, "wb") as f:
        f.write(blob)
    kdata = entries[2][1]
    manifest = os.path.splitext(out)[0] + ".txt"
    with open(manifest, "w") as f:
        f.write("build=%s\nsize=%d\nsha256=%s\nkernel-size=%d\nkernel-sha256=%s\n" %
                (build, len(blob), hashlib.sha256(blob).hexdigest(), len(kdata), hashlib.sha256(kdata).hexdigest()))


main()
