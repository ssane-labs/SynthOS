#!/usr/bin/env python3
"""Copies files out of a Debian package (ar archive with data.tar.xz/gz) without dpkg.
usage: debfiles.py PACKAGE.deb OUTDIR path/in/package[=name] ..."""
import io, os, sys, tarfile

deb, outdir, wanted = sys.argv[1], sys.argv[2], sys.argv[3:]
data = open(deb, "rb").read()
if data[:8] != b"!<arch>\n":
    sys.exit("debfiles: %s is not a Debian package" % deb)
pos, payload = 8, None
while pos + 60 <= len(data):
    name = data[pos:pos + 16].decode().strip().rstrip("/")
    size = int(data[pos + 48:pos + 58])
    if name.startswith("data.tar"):
        payload = data[pos + 60:pos + 60 + size]
    pos += 60 + size + (size & 1)
if payload is None:
    sys.exit("debfiles: no data archive in %s" % deb)
os.makedirs(outdir, exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(payload)) as tar:
    members = {m.name.lstrip("./"): m for m in tar.getmembers()}
    for item in wanted:
        path, _, rename = item.partition("=")
        m = members.get(path)
        if m is None or not m.isfile():
            sys.exit("debfiles: %s is not in %s" % (path, deb))
        with open(os.path.join(outdir, rename or os.path.basename(path)), "wb") as f:
            f.write(tar.extractfile(m).read())
