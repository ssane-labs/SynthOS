"""A WireGuard server for the tests: answers handshakes on a UDP port of the host (10.0.2.2 inside QEMU) and, inside
the tunnel, is 10.99.0.1: pings, DNS (vpn.test is 10.99.0.1) and HTTP on port 80 with a tiny TCP.
Usage: wgpeer.py --config PORT   prints the client configuration
       wgpeer.py PORT            runs the server
"""
import hashlib
import hmac
import os
import socket
import struct
import sys
from base64 import b64encode

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

SERVER = X25519PrivateKey.from_private_bytes(hashlib.sha256(b"synthos wg server").digest())
CLIENT = X25519PrivateKey.from_private_bytes(hashlib.sha256(b"synthos wg client").digest())
PSK = hashlib.sha256(b"synthos wg psk").digest()
INNER = bytes([10, 99, 0, 1])
BODY = b"hello through the tunnel\n"


def raw(key):
    return key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def b64(data):
    return b64encode(data).decode()


if sys.argv[1] == "--config":
    priv = hashlib.sha256(b"synthos wg client").digest()
    print("[Interface]\nPrivateKey = %s\nAddress = 10.99.0.2/32, fd00:99::2/128\nDNS = 10.99.0.1\n\n"
          "[Peer]\nPublicKey = %s\nPresharedKey = %s\nAllowedIPs = 10.99.0.0/24\nEndpoint = 10.0.2.2:%s\n"
          "PersistentKeepalive = 25" % (b64(priv), b64(raw(SERVER)), b64(PSK), sys.argv[2]))
    sys.exit(0)

S_PUB = raw(SERVER)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", int(sys.argv[1])))


def blake(*parts):
    h = hashlib.blake2s()
    for p in parts:
        h.update(p)
    return h.digest()


def kdf(n, key, data):
    prk = hmac.new(key, data, hashlib.blake2s).digest()
    out, t = [], b""
    for i in range(1, n + 1):
        t = hmac.new(prk, t + bytes([i]), hashlib.blake2s).digest()
        out.append(t)
    return out


def aead(key, counter, data, aad, decrypt=False):
    box, nonce = ChaCha20Poly1305(key), b"\0" * 4 + struct.pack("<Q", counter)
    return box.decrypt(nonce, data, aad) if decrypt else box.encrypt(nonce, data, aad)


def dh(priv, pub):
    return priv.exchange(X25519PublicKey.from_public_bytes(pub))


def mac(key, data):
    return hashlib.blake2s(data, digest_size=16, key=key).digest()


session = {}
last_stamp = [b""]


def handshake(msg, addr):
    if len(msg) != 148 or mac(blake(b"mac1----", S_PUB), msg[:116]) != msg[116:132]:
        return
    c = blake(b"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s")
    h = blake(blake(c, b"WireGuard v1 zx2c4 Jason@zx2c4.com"), S_PUB)
    e_i = msg[8:40]
    c = kdf(1, c, e_i)[0]
    h = blake(h, e_i)
    c, k = kdf(2, c, dh(SERVER, e_i))
    s_i = aead(k, 0, msg[40:88], h, True)
    h = blake(h, msg[40:88])
    c, k = kdf(2, c, dh(SERVER, s_i))
    stamp = aead(k, 0, msg[88:116], h, True)
    h = blake(h, msg[88:116])
    if s_i != raw(CLIENT) or stamp <= last_stamp[0]:
        return
    last_stamp[0] = stamp
    eph = X25519PrivateKey.generate()
    e_r = raw(eph)
    index = os.urandom(4)
    c = kdf(1, c, e_r)[0]
    h = blake(h, e_r)
    c = kdf(1, c, dh(eph, e_i))[0]
    c = kdf(1, c, dh(eph, s_i))[0]
    c, tau, k = kdf(3, c, PSK)
    h = blake(h, tau)
    empty = aead(k, 0, b"", h)
    out = b"\x02\0\0\0" + index + msg[4:8] + e_r + empty
    out += mac(blake(b"mac1----", s_i), out) + bytes(16)
    recv, send = kdf(2, c, b"")
    session.update(index=index, peer=msg[4:8], recv=recv, send=send, counter=0, addr=addr)
    sock.sendto(out, addr)


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def send_ip(dst, proto, payload):
    if proto in (6, 17):
        at = 16 if proto == 6 else 6
        pseudo = INNER + dst + struct.pack("!BBH", 0, proto, len(payload))
        payload = bytearray(payload)
        payload[at:at + 2] = struct.pack("!H", checksum(pseudo + bytes(payload)))
        payload = bytes(payload)
    elif proto == 1:
        payload = bytearray(payload)
        payload[2:4] = struct.pack("!H", checksum(bytes(payload)))
        payload = bytes(payload)
    hdr = bytearray(struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0x4000, 64, proto, 0, INNER, dst))
    hdr[10:12] = struct.pack("!H", checksum(bytes(hdr)))
    plain = bytes(hdr) + payload
    plain += bytes(-len(plain) % 16)
    n = session["counter"]
    session["counter"] += 1
    sock.sendto(b"\x04\0\0\0" + session["peer"] + struct.pack("<Q", n) + aead(session["send"], n, plain, b""),
                session["addr"])


def dns_answer(q):
    ident = struct.unpack("!H", q[:2])[0]
    at, labels = 12, []
    while q[at]:
        labels.append(q[at + 1:at + 1 + q[at]].decode())
        at += 1 + q[at]
    qtype = struct.unpack("!H", q[at + 1:at + 3])[0]
    question = q[12:at + 5]
    if ".".join(labels).lower() != "vpn.test":
        return struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0) + question
    if qtype != 1:
        return struct.pack("!HHHHHH", ident, 0x8180, 1, 0, 0, 0) + question
    answer = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 300, 4) + INNER
    return struct.pack("!HHHHHH", ident, 0x8180, 1, 1, 0, 0) + question + answer


conns = {}


def tcp(src, seg):
    sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", seg[:14])
    data = seg[(off >> 4) * 4:]

    def reply(f, s, a, payload=b"", opts=b""):
        hdr = struct.pack("!HHIIBBHHH", dport, sport, s, a, (5 + len(opts) // 4) << 4, f, 65535, 0, 0) + opts
        send_ip(src, 6, hdr + payload)

    if flags & 0x04:
        conns.pop(sport, None)
        return
    if flags & 0x02:
        conns[sport] = {"next": seq + 1, "mine": 1001, "sent": False}
        reply(0x12, 1000, seq + 1, opts=struct.pack("!BBH", 2, 4, 1380))
        return
    c = conns.get(sport)
    if not c:
        reply(0x04, ack, 0)
        return
    if data and seq == c["next"]:
        c["next"] += len(data)
        if b"\r\n\r\n" in data and not c["sent"]:
            c["sent"] = True
            body = b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(BODY) + BODY
            reply(0x19, c["mine"], c["next"], body)
            c["mine"] += len(body) + 1
        else:
            reply(0x10, c["mine"], c["next"])
    if flags & 0x01:
        c["next"] = seq + len(data) + 1
        reply(0x10, c["mine"], c["next"])
        conns.pop(sport, None)


def inner(packet):
    if len(packet) < 20 or packet[0] >> 4 != 4 or packet[16:20] != INNER:
        return
    hl, total, proto, src = (packet[0] & 15) * 4, struct.unpack("!H", packet[2:4])[0], packet[9], packet[12:16]
    payload = packet[hl:total]
    if proto == 1 and payload[0] == 8:
        send_ip(src, 1, b"\0\0\0\0" + payload[4:])
    elif proto == 17 and struct.unpack("!H", payload[2:4])[0] == 53:
        answer = dns_answer(payload[8:])
        send_ip(src, 17, struct.pack("!HHHH", 53, struct.unpack("!H", payload[:2])[0], 8 + len(answer), 0) + answer)
    elif proto == 6 and struct.unpack("!H", payload[2:4])[0] == 80:
        tcp(src, payload)


while True:
    msg, addr = sock.recvfrom(4096)
    try:
        if msg[:4] == b"\x01\0\0\0":
            handshake(msg, addr)
        elif msg[:4] == b"\x04\0\0\0" and session and msg[4:8] == session["index"]:
            counter = struct.unpack("<Q", msg[8:16])[0]
            plain = aead(session["recv"], counter, msg[16:], b"", True)
            if plain:
                inner(plain)
    except Exception:
        pass
