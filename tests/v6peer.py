"""An IPv6 neighbour for the tests, on the other end of a QEMU socket network card (Ethernet frames in UDP).

It is the router of 2001:db8:1::/64 (router advertisements with the prefix and a DNS server), answers neighbour
solicitations and pings, AAAA questions for six.test, and HTTP on port 80 of 2001:db8:1::1 with a tiny TCP.
Usage: v6peer.py LISTEN_PORT QEMU_PORT
"""
import socket
import struct
import sys

MAC = bytes.fromhex("020000000001")
LL = bytes.fromhex("fe800000000000000000000000000001")
PREFIX = bytes.fromhex("20010db8000100000000000000000000")
HOST = PREFIX[:15] + b"\x01"
DNS = PREFIX[:14] + b"\x00\x53"
OURS = (LL, HOST, DNS)
BODY = b"hello over ipv6\n"

listen, qemu = int(sys.argv[1]), int(sys.argv[2])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", listen))


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def upper(src, dst, proto, payload, at):
    pseudo = src + dst + struct.pack("!IxxxB", len(payload), proto)
    payload = bytearray(payload)
    payload[at:at + 2] = b"\0\0"
    payload[at:at + 2] = struct.pack("!H", checksum(pseudo + bytes(payload)))
    return bytes(payload)


def send(dst_mac, src, dst, proto, payload, hops=64):
    at = {58: 2, 6: 16, 17: 6}[proto]
    payload = upper(src, dst, proto, payload, at)
    ip = struct.pack("!IHBB", 6 << 28, len(payload), proto, hops) + src + dst + payload
    sock.sendto(dst_mac + MAC + b"\x86\xdd" + ip, ("127.0.0.1", qemu))


def advert(dst_mac, dst):
    ra = struct.pack("!BBHBBHII", 134, 0, 0, 64, 0, 1800, 0, 0)
    ra += struct.pack("!BB", 1, 1) + MAC
    ra += struct.pack("!BBBBIII", 3, 4, 64, 0xC0, 86400, 14400, 0) + PREFIX
    ra += struct.pack("!BBHI", 25, 3, 0, 3600) + DNS
    send(dst_mac, LL, dst, 58, ra, 255)


def dns_answer(q):
    ident, _, qd = struct.unpack("!HHH", q[:6])
    at, labels = 12, []
    while q[at]:
        labels.append(q[at + 1:at + 1 + q[at]].decode())
        at += 1 + q[at]
    qtype = struct.unpack("!H", q[at + 1:at + 3])[0]
    question = q[12:at + 5]
    name = ".".join(labels).lower()
    if name != "six.test":
        return struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0) + question
    if qtype != 28:
        return struct.pack("!HHHHHH", ident, 0x8180, 1, 0, 0, 0) + question
    answer = b"\xc0\x0c" + struct.pack("!HHIH", 28, 1, 300, 16) + HOST
    return struct.pack("!HHHHHH", ident, 0x8180, 1, 1, 0, 0) + question + answer


conns = {}


def tcp(mac, src, dst, seg):
    sport, dport, seq, ack, off, flags = struct.unpack("!HHIIBB", seg[:14])
    data = seg[(off >> 4) * 4:]
    key = (src, sport)

    def reply(f, s, a, payload=b"", opts=b""):
        hdr = struct.pack("!HHIIBBHHH", dport, sport, s, a, (5 + len(opts) // 4) << 4, f, 65535, 0, 0) + opts
        send(mac, dst, src, 6, hdr + payload)

    if flags & 0x04:
        conns.pop(key, None)
        return
    if flags & 0x02:
        conns[key] = {"next": seq + 1, "mine": 1001, "sent": False}
        reply(0x12, 1000, seq + 1, opts=struct.pack("!BBH", 2, 4, 1440))
        return
    c = conns.get(key)
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
        conns.pop(key, None)


while True:
    frame, _ = sock.recvfrom(2048)
    if len(frame) < 54 or frame[12:14] != b"\x86\xdd":
        continue
    mac, ip = frame[6:12], frame[14:]
    length, proto, hops = struct.unpack("!HBB", ip[4:8])
    src, dst, payload = ip[8:24], ip[24:40], ip[40:40 + length]
    if proto == 58:
        kind = payload[0]
        if kind == 133:
            advert(mac, src if src != bytes(16) else bytes.fromhex("ff020000000000000000000000000001"))
        elif kind == 135 and payload[8:24] in OURS:
            na = struct.pack("!BBHI", 136, 0, 0, 0x60000000) + payload[8:24] + struct.pack("!BB", 2, 1) + MAC
            send(mac, payload[8:24], src, 58, na, 255)
        elif kind == 128 and dst in OURS:
            send(mac, dst, src, 58, bytes([129]) + payload[1:])
    elif proto == 17 and dst == DNS and len(payload) > 8 and struct.unpack("!H", payload[2:4])[0] == 53:
        sport = struct.unpack("!H", payload[:2])[0]
        answer = dns_answer(payload[8:])
        send(mac, DNS, src, 17, struct.pack("!HHHH", 53, sport, 8 + len(answer), 0) + answer)
    elif proto == 6 and dst == HOST:
        tcp(mac, src, dst, payload)
