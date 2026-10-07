"""LZ4 block compressor and the image plane filter used for SynthOS assets (decoder: kernel/lib/lz4.c)."""


def compress(data):
    data = bytes(data)
    n = len(data)
    out = bytearray()
    table = {}
    anchor = i = 0
    limit = n - 12
    while i < limit:
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > 0xFFFF:
            i += 1
            continue
        length = 4
        while i + length < n - 5 and data[cand + length] == data[i + length]:
            length += 1
        while i > anchor and cand > 0 and data[i - 1] == data[cand - 1]:
            i -= 1
            cand -= 1
            length += 1
        emit(out, data[anchor:i], i - cand, length)
        for k in range(i + 1, min(i + length, limit)):
            table[data[k:k + 4]] = k
        i += length
        anchor = i
    emit(out, data[anchor:], 0, 0)
    return bytes(out)


def emit(out, literals, offset, length):
    lit = len(literals)
    ml = length - 4 if length else 0
    token = (min(lit, 15) << 4) | (min(ml, 15) if length else 0)
    out.append(token)
    if lit >= 15:
        rest = lit - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    out += literals
    if not length:
        return
    out += bytes([offset & 0xFF, offset >> 8])
    if ml >= 15:
        rest = ml - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)


def planes(pixels, width):
    """ARGB words -> four delta-coded byte planes (A, R, G, B), which compress far better."""
    out = bytearray()
    for shift in (24, 16, 8, 0):
        for row in range(0, len(pixels), width):
            prev = 0
            for p in pixels[row:row + width]:
                v = (p >> shift) & 0xFF
                out.append((v - prev) & 0xFF)
                prev = v
    return bytes(out)


def alpha_rows(values, width):
    out = bytearray()
    for row in range(0, len(values), width):
        prev = 0
        for v in values[row:row + width]:
            out.append((v - prev) & 0xFF)
            prev = v
    return bytes(out)
