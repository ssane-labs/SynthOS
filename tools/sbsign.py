#!/usr/bin/env python3
"""Seal and sign the UEFI loader for Secure Boot.

usage: sbsign.py --key KEY.pem [--pass PASSWORD] --cert CERT.pem [--seal KERNEL.BIN] IN.EFI OUT.EFI
       sbsign.py --hash IN.EFI          print the Authenticode SHA-256 of an image

--seal writes the SHA-256 of the kernel into the loader (after the "SynthOSKernelSum" tag), so that under Secure
Boot the signed loader starts only that kernel. The signature is an Authenticode PKCS#7 SignedData (SHA-256, RSA)
in the image's certificate table, which is what UEFI firmware checks against its db.
"""
import argparse
import hashlib
import struct
import sys

TAG = b"SynthOSKernelSum"


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, content):
    return bytes([tag]) + der_len(len(content)) + content


def seq(*items):
    return tlv(0x30, b"".join(items))


def set_of(*items):
    return tlv(0x31, b"".join(sorted(items)))


def value_of(der):
    n = der[1]
    return der[2:] if n < 0x80 else der[2 + (n & 0x7F):]


def ctx(n, content):
    return tlv(0xA0 | n, content)


def integer(v):
    b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big", signed=True)
    return tlv(0x02, b)


def oid(dotted):
    parts = [int(p) for p in dotted.split(".")]
    out = bytes([40 * parts[0] + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        p >>= 7
        while p:
            chunk.append(0x80 | (p & 0x7F))
            p >>= 7
        out += bytes(reversed(chunk))
    return tlv(0x06, out)


NULL = b"\x05\x00"
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RSA = seq(oid("1.2.840.113549.1.1.1"), NULL)
SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"
SPC_PE_IMAGE_DATA = "1.3.6.1.4.1.311.2.1.15"


class Image:
    def __init__(self, data):
        self.data = bytearray(data)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        self.sections = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        self.opt = pe + 24
        magic = struct.unpack_from("<H", data, self.opt)[0]
        dirs = self.opt + (112 if magic == 0x20B else 96)
        self.checksum = self.opt + 64
        self.cert_dir = dirs + 4 * 8
        self.headers = struct.unpack_from("<I", data, self.opt + 60)[0]
        self.section_table = self.opt + opt_size

    def cert_table(self):
        return struct.unpack_from("<II", self.data, self.cert_dir)

    def strip(self):
        off, size = self.cert_table()
        if off and size and off + size == len(self.data):
            del self.data[off:]
        struct.pack_into("<II", self.data, self.cert_dir, 0, 0)

    def digest(self):
        d = self.data
        h = hashlib.sha256()
        h.update(d[:self.checksum])
        h.update(d[self.checksum + 4:self.cert_dir])
        h.update(d[self.cert_dir + 8:self.headers])
        done = self.headers
        sections = []
        for i in range(self.sections):
            s = self.section_table + 40 * i
            size, ptr = struct.unpack_from("<II", d, s + 16)
            if size:
                sections.append((ptr, size))
        for ptr, size in sorted(sections):
            h.update(d[ptr:ptr + size])
            done += size
        off, size = self.cert_table()
        end = len(d) - (size if off else 0)
        if end > done:
            h.update(d[done:end])
        return h.digest()


def seal(image, kernel):
    at = image.data.find(TAG)
    if at < 0 or image.data.find(TAG, at + 1) >= 0:
        raise ValueError("the loader has no single seal tag")
    image.data[at + len(TAG):at + len(TAG) + 32] = hashlib.sha256(kernel).digest()


def sign(image, key, cert):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    image.strip()
    while len(image.data) % 8:
        image.data.append(0)
    pe_data = seq(tlv(0x03, b"\x00"), ctx(0, ctx(2, tlv(0x80, "<<<Obsolete>>>".encode("utf-16-be")))))
    indirect = seq(seq(oid(SPC_PE_IMAGE_DATA), pe_data), seq(SHA256, tlv(0x04, image.digest())))
    body = value_of(indirect)
    attrs = [seq(oid("1.2.840.113549.1.9.3"), set_of(oid(SPC_INDIRECT_DATA))),
             seq(oid("1.2.840.113549.1.9.4"), set_of(tlv(0x04, hashlib.sha256(body).digest())))]
    signed_attrs = set_of(*attrs)
    signature = key.sign(signed_attrs, padding.PKCS1v15(), hashes.SHA256())
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signer = seq(integer(1), seq(cert.issuer.public_bytes(), integer(cert.serial_number)), SHA256,
                 ctx(0, value_of(signed_attrs)), RSA, tlv(0x04, signature))
    signed_data = seq(integer(1), set_of(SHA256), seq(oid(SPC_INDIRECT_DATA), ctx(0, indirect)), ctx(0, cert_der),
                      set_of(signer))
    p7 = seq(oid("1.2.840.113549.1.7.2"), ctx(0, signed_data))
    entry = struct.pack("<IHH", 8 + len(p7), 0x0200, 0x0002) + p7
    while len(entry) % 8:
        entry += b"\0"
    off = len(image.data)
    image.data += entry
    struct.pack_into("<II", image.data, image.cert_dir, off, len(entry))


def main():
    ap = argparse.ArgumentParser(description="seal and sign the SynthOS UEFI loader")
    ap.add_argument("--key")
    ap.add_argument("--cert")
    ap.add_argument("--pass", dest="password", help="the password of an encrypted key")
    ap.add_argument("--seal")
    ap.add_argument("--hash", action="store_true")
    ap.add_argument("input")
    ap.add_argument("output", nargs="?")
    a = ap.parse_args()
    image = Image(open(a.input, "rb").read())
    if a.hash:
        print(image.digest().hex())
        return 0
    if not a.output:
        ap.error("an output file is needed")
    if a.seal:
        seal(image, open(a.seal, "rb").read())
    if a.key and a.cert:
        from cryptography import x509
        from cryptography.hazmat.primitives import serialization
        password = a.password.encode() if a.password else None
        key = serialization.load_pem_private_key(open(a.key, "rb").read(), password=password)
        cert = x509.load_pem_x509_certificate(open(a.cert, "rb").read())
        sign(image, key, cert)
    open(a.output, "wb").write(bytes(image.data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
