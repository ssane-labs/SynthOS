#!/usr/bin/env python3
"""A tiny GGUF language model with random weights, and what SynthOS's assistant engine must compute with it.

usage: aimodel.py OUTDIR

Writes OUTDIR/tiny.gguf (Qwen2), tiny3.gguf (Qwen3) and tinyl.gguf (Llama), a tiny Whisper speech model
(whisper.bin, the whisper.cpp format) with a sound for it to hear (speech.wav), and OUTDIR/cases.txt: the commands
for tests/run.sh and what each must print (the tokens of some texts, the next tokens greedy decoding picks). Each
model has the layout of a real one (tensor names, metadata, a byte-level BPE vocabulary with merges and the ChatML
special tokens), and its tensors use every quantisation the engine reads: F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q2_K .. Q6_K. The block layouts follow
ggml (ggml-common.h); quantised blocks hold random codes and scales, so the reference only has to dequantise them.
"""
import math
import struct
import sys

import numpy as np

QK_K = 256

# ---- GGUF writer ----

T_U32, T_I32, T_F32, T_BOOL, T_STR, T_ARR, T_U64 = 4, 5, 6, 7, 8, 9, 10
GGML = {"F32": 0, "F16": 1, "Q4_0": 2, "Q4_1": 3, "Q5_0": 6, "Q5_1": 7, "Q8_0": 8, "Q2_K": 10, "Q3_K": 11,
        "Q4_K": 12, "Q5_K": 13, "Q6_K": 14, "BF16": 30}
BLOCK = {"F32": (1, 4), "F16": (1, 2), "BF16": (1, 2), "Q4_0": (32, 18), "Q4_1": (32, 20), "Q5_0": (32, 22),
         "Q5_1": (32, 24), "Q8_0": (32, 34), "Q2_K": (256, 84), "Q3_K": (256, 110), "Q4_K": (256, 144),
         "Q5_K": (256, 176), "Q6_K": (256, 210)}


def gstr(s):
    b = s.encode()
    return struct.pack("<Q", len(b)) + b


def kv(key, typ, value):
    out = gstr(key) + struct.pack("<I", typ)
    if typ == T_U32:
        out += struct.pack("<I", value)
    elif typ == T_I32:
        out += struct.pack("<i", value)
    elif typ == T_F32:
        out += struct.pack("<f", value)
    elif typ == T_BOOL:
        out += struct.pack("<B", value)
    elif typ == T_STR:
        out += gstr(value)
    elif typ == T_ARR:
        sub, items = value
        out += struct.pack("<IQ", sub, len(items))
        for it in items:
            out += gstr(it) if sub == T_STR else struct.pack("<i" if sub == T_I32 else "<f", it)
    return out


def write_gguf(path, meta, tensors):
    head = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(meta))
    for m in meta:
        head += kv(*m)
    data = b""
    infos = b""
    for name, typ, dims, raw in tensors:
        while len(data) % 32:
            data += b"\0"
        infos += gstr(name) + struct.pack("<I", len(dims)) + b"".join(struct.pack("<Q", d) for d in dims)
        infos += struct.pack("<IQ", GGML[typ], len(data))
        data += raw
    blob = head + infos
    while len(blob) % 32:
        blob += b"\0"
    open(path, "wb").write(blob + data)


# ---- dequantisation, as ggml does it ----

def f16(b, o):
    return float(np.frombuffer(b[o:o + 2], dtype="<f2")[0])


def scale_min_k4(j, q):
    if j < 4:
        return q[j] & 63, q[j + 4] & 63
    return (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4), (q[j + 4] >> 4) | ((q[j] >> 6) << 4)


def deq_block(typ, b):
    y = []
    if typ == "Q4_0":
        d = f16(b, 0)
        qs = b[2:18]
        y = [((qs[j] & 15) - 8) * d for j in range(16)] + [((qs[j] >> 4) - 8) * d for j in range(16)]
    elif typ == "Q4_1":
        d, m = f16(b, 0), f16(b, 2)
        qs = b[4:20]
        y = [(qs[j] & 15) * d + m for j in range(16)] + [(qs[j] >> 4) * d + m for j in range(16)]
    elif typ in ("Q5_0", "Q5_1"):
        d = f16(b, 0)
        m = f16(b, 2) if typ == "Q5_1" else 0.0
        o = 4 if typ == "Q5_1" else 2
        qh = struct.unpack("<I", b[o:o + 4])[0]
        qs = b[o + 4:o + 20]
        off = 16 if typ == "Q5_0" else 0
        lo, hi = [], []
        for j in range(16):
            xh0 = ((qh >> j) << 4) & 0x10
            xh1 = (qh >> (j + 12)) & 0x10
            lo.append((((qs[j] & 15) | xh0) - off) * d + m)
            hi.append((((qs[j] >> 4) | xh1) - off) * d + m)
        y = lo + hi
    elif typ == "Q8_0":
        d = f16(b, 0)
        y = [v * d for v in np.frombuffer(b[2:34], dtype=np.int8)]
    elif typ == "Q2_K":
        sc, qs, d, mn = b[0:16], b[16:80], f16(b, 80), f16(b, 82)
        isc = 0
        for n in range(0, QK_K, 128):
            q = qs[n // 4:n // 4 + 32]
            shift = 0
            for j in range(4):
                for half in range(2):
                    s = sc[isc]
                    isc += 1
                    dl, ml = d * (s & 15), mn * (s >> 4)
                    y += [dl * ((q[l + 16 * half] >> shift) & 3) - ml for l in range(16)]
                shift += 2
    elif typ == "Q3_K":
        hm, qs, raw, d = b[0:32], b[32:96], b[96:108], f16(b, 108)
        aux = list(struct.unpack("<3I", raw)) + [0]
        tmp = aux[2]
        k1, k2 = 0x03030303, 0x0F0F0F0F
        aux[2] = ((aux[0] >> 4) & k2) | (((tmp >> 4) & k1) << 4)
        aux[3] = ((aux[1] >> 4) & k2) | (((tmp >> 6) & k1) << 4)
        aux[0] = (aux[0] & k2) | (((tmp >> 0) & k1) << 4)
        aux[1] = (aux[1] & k2) | (((tmp >> 2) & k1) << 4)
        scales = np.frombuffer(struct.pack("<4I", *aux), dtype=np.int8)
        isc, m = 0, 1
        for n in range(0, QK_K, 128):
            q = qs[n // 4:n // 4 + 32]
            shift = 0
            for j in range(4):
                for half in range(2):
                    dl = d * (int(scales[isc]) - 32)
                    isc += 1
                    y += [dl * (((q[l + 16 * half] >> shift) & 3) - (0 if hm[l + 16 * half] & m else 4))
                          for l in range(16)]
                shift += 2
                m <<= 1
    elif typ in ("Q4_K", "Q5_K"):
        d, mn, sc = f16(b, 0), f16(b, 2), b[4:16]
        if typ == "Q4_K":
            qh, ql = None, b[16:144]
        else:
            qh, ql = b[16:48], b[48:176]
        isc, u1, u2 = 0, 1, 2
        for j in range(0, QK_K, 64):
            s1, m1 = scale_min_k4(isc, sc)
            s2, m2 = scale_min_k4(isc + 1, sc)
            q = ql[j // 2:j // 2 + 32]
            for l in range(32):
                h = 16 if qh is not None and qh[l] & u1 else 0
                y.append(d * s1 * ((q[l] & 15) + h) - mn * m1)
            for l in range(32):
                h = 16 if qh is not None and qh[l] & u2 else 0
                y.append(d * s2 * ((q[l] >> 4) + h) - mn * m2)
            isc += 2
            u1 <<= 2
            u2 <<= 2
    elif typ == "Q6_K":
        ql, qh, sc, d = b[0:128], b[128:192], np.frombuffer(b[192:208], dtype=np.int8), f16(b, 208)
        out = [0.0] * QK_K
        for n in range(0, QK_K, 128):
            L, H, S = ql[n // 2:n // 2 + 64], qh[n // 4:n // 4 + 32], sc[n // 16:n // 16 + 8]
            for l in range(32):
                i = l // 16
                q1 = ((L[l] & 15) | (((H[l] >> 0) & 3) << 4)) - 32
                q2 = ((L[l + 32] & 15) | (((H[l] >> 2) & 3) << 4)) - 32
                q3 = ((L[l] >> 4) | (((H[l] >> 4) & 3) << 4)) - 32
                q4 = ((L[l + 32] >> 4) | (((H[l] >> 6) & 3) << 4)) - 32
                out[n + l] = d * S[i] * q1
                out[n + l + 32] = d * S[i + 2] * q2
                out[n + l + 64] = d * S[i + 4] * q3
                out[n + l + 96] = d * S[i + 6] * q4
        y = out
    return y


def dequant(typ, raw, n):
    if typ == "F32":
        return np.frombuffer(raw, dtype="<f4").astype(np.float64)
    if typ == "F16":
        return np.frombuffer(raw, dtype="<f2").astype(np.float64)
    if typ == "BF16":
        u = np.frombuffer(raw, dtype="<u2").astype(np.uint32) << 16
        return u.view(np.float32).astype(np.float64)
    per, size = BLOCK[typ]
    out = []
    for i in range(n // per):
        out += deq_block(typ, raw[i * size:(i + 1) * size])
    return np.array(out, dtype=np.float64)


def make_raw(rng, typ, n, scale):
    """random tensor bytes of n values that dequantise to about +-scale"""
    if typ == "F32":
        return (rng.standard_normal(n) * scale).astype("<f4").tobytes()
    if typ == "F16":
        return (rng.standard_normal(n) * scale).astype("<f2").tobytes()
    if typ == "BF16":
        f = (rng.standard_normal(n) * scale).astype("<f4")
        return (f.view("<u4") >> 16).astype("<u2").tobytes()
    per, size = BLOCK[typ]
    out = bytearray()
    for _ in range(n // per):
        blk = bytearray(rng.integers(0, 256, size, dtype=np.uint8).tobytes())

        def put16(o, v):
            blk[o:o + 2] = np.array([v], dtype="<f2").tobytes()

        if typ in ("Q4_0", "Q8_0"):
            put16(0, scale / (8 if typ == "Q4_0" else 127) * rng.uniform(0.5, 1.5))
        elif typ in ("Q4_1", "Q5_1"):
            put16(0, scale / (8 if typ == "Q4_1" else 16) * rng.uniform(0.5, 1.5))
            put16(2, -scale * rng.uniform(0.5, 1.5))
        elif typ == "Q5_0":
            put16(0, scale / 16 * rng.uniform(0.5, 1.5))
        elif typ == "Q2_K":
            put16(80, scale / 30 * rng.uniform(0.5, 1.5))
            put16(82, scale / 15 * rng.uniform(0.5, 1.5))
        elif typ == "Q3_K":
            put16(108, scale / 64 * rng.uniform(0.5, 1.5))
        elif typ in ("Q4_K", "Q5_K"):
            put16(0, scale / 300 * rng.uniform(0.5, 1.5))
            put16(2, scale / 40 * rng.uniform(0.5, 1.5))
        elif typ == "Q6_K":
            blk[192:208] = rng.integers(-64, 64, 16, dtype=np.int8).tobytes()
            put16(208, scale / 2000 * rng.uniform(0.5, 1.5))
        out += blk
    return bytes(out)


# ---- byte-level BPE (GPT-2 style), the vocabulary and the reference tokenizer ----

def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(0xA1, 0xAD)) + list(range(0xAE, 0x100))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, [chr(c) for c in cs]))


B2U = bytes_to_unicode()
U2B = {v: k for k, v in B2U.items()}
SPECIALS = ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]

CORPUS = ("the quick brown fox jumps over the lazy dog. hello world, hello there! open the file and change the "
          "settings of the system. what is the weather today? the assistant reads the page and writes a summary. "
          "Привет, мир! Это тест. 12345 numbers 2024. ") * 3


def is_letter(ch):
    o = ord(ch)
    if o < 128:
        return ch.isalpha()
    return not (0x2000 <= o <= 0x206F or 0x3000 <= o <= 0x303F or 0xFF00 <= o <= 0xFF0F or o in (0xA0, 0x85)
                or 0x80 <= o < 0xA0 or 0xA1 <= o <= 0xBF)


def is_number(ch):
    return "0" <= ch <= "9"


def is_space(ch):
    return ch in " \t\n\r\f\v" or ch in "   　"


def pretokenize(text, digits=1):
    """the Qwen2 split, as the engine approximates it: contractions, letters with one leading non-letter, single
    digits, punctuation runs with a leading space, newlines, spaces"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "'" and i + 1 < n:
            low = text[i + 1:i + 3].lower()
            hit = next((s for s in ("re", "ve", "ll") if low.startswith(s)), None)
            if hit is None and low[:1] in ("s", "t", "m", "d"):
                hit = low[:1]
            if hit:
                out.append(text[i:i + 1 + len(hit)])
                i += 1 + len(hit)
                continue
        j = i
        if not (c in "\r\n" or is_letter(c) or is_number(c)) and i + 1 < n and is_letter(text[i + 1]):
            j = i + 1
        if is_letter(text[j]):
            k = j
            while k < n and is_letter(text[k]):
                k += 1
            out.append(text[i:k])
            i = k
            continue
        if is_number(c):
            k = i
            while k < n and k - i < digits and is_number(text[k]):
                k += 1
            out.append(text[i:k])
            i = k
            continue
        j = i + 1 if c == " " and i + 1 < n and not (is_space(text[i + 1]) or is_letter(text[i + 1])
                                                      or is_number(text[i + 1])) else i
        if not (is_space(text[j]) or is_letter(text[j]) or is_number(text[j])):
            k = j
            while k < n and not (is_space(text[k]) or is_letter(text[k]) or is_number(text[k])):
                k += 1
            while k < n and text[k] in "\r\n":
                k += 1
            out.append(text[i:k])
            i = k
            continue
        # whitespace: \s*[\r\n]+ | \s+(?!\S) | \s+
        k = i
        while k < n and is_space(text[k]):
            k += 1
        last_nl = max((p for p in range(i, k) if text[p] in "\r\n"), default=-1)
        if last_nl >= 0:
            out.append(text[i:last_nl + 1])
            i = last_nl + 1
            continue
        if k < n and k - i > 1:
            out.append(text[i:k - 1])
            i = k - 1
            continue
        out.append(text[i:k])
        i = k
    return out


def train_bpe(nmerges):
    words = {}
    for w in pretokenize(CORPUS):
        key = tuple(B2U[b] for b in w.encode())
        words[key] = words.get(key, 0) + 1
    merges = []
    for _ in range(nmerges):
        pairs = {}
        for w, c in words.items():
            for a, b in zip(w, w[1:]):
                pairs[(a, b)] = pairs.get((a, b), 0) + c
        if not pairs:
            break
        best = max(sorted(pairs), key=lambda p: pairs[p])
        merges.append(best)
        new = {}
        for w, c in words.items():
            out, i = [], 0
            while i < len(w):
                if i + 1 < len(w) and (w[i], w[i + 1]) == best:
                    out.append(w[i] + w[i + 1])
                    i += 2
                else:
                    out.append(w[i])
                    i += 1
            new[tuple(out)] = new.get(tuple(out), 0) + c
        words = new
    return merges


def bpe(word, ranks):
    parts = list(word)
    while len(parts) > 1:
        best, at = None, -1
        for i in range(len(parts) - 1):
            r = ranks.get((parts[i], parts[i + 1]))
            if r is not None and (best is None or r < best):
                best, at = r, i
        if best is None:
            break
        parts[at:at + 2] = [parts[at] + parts[at + 1]]
    return parts


def tokenize(text, vocab, ranks, digits=1, whole=False):
    ids = []
    i = 0
    while i < len(text):
        hit = next((s for s in SPECIALS if text.startswith(s, i)), None)
        nxt = min([p for p in (text.find(s, i) for s in SPECIALS) if p >= 0] or [len(text)])
        if hit:
            ids.append(vocab[hit])
            i += len(hit)
            continue
        for w in pretokenize(text[i:nxt], digits):
            enc = "".join(B2U[b] for b in w.encode())
            if whole and len(enc) > 1 and enc in vocab:  # ignore_merges (MiniCPM5, Llama 3)
                ids.append(vocab[enc])
                continue
            ids += [vocab[p] for p in bpe(enc, ranks)]
        i = nxt
    return ids


# ---- the model ----

# ---- the engine's integer path: inputs of quantised matrices go to 8 bits, a scale per 32 values ----

INT_TYPES = {"Q4_0", "Q4_1", "Q5_0", "Q5_1", "Q8_0", "Q2_K", "Q3_K", "Q4_K", "Q5_K", "Q6_K"}


def qdq(x):
    """x as the engine sees it after quant_act: rounded to 8 bits with amax/127 per block of 32 (float32)"""
    x = np.asarray(x, dtype=np.float32)
    v = x.reshape(-1, 32)
    d = (np.abs(v).max(1, keepdims=True) / np.float32(127)).astype(np.float32)
    inv = np.where(d > 0, np.float32(1) / np.where(d > 0, d, 1), 0).astype(np.float32)
    s = (v * inv).astype(np.float32)
    q = np.sign(s) * np.floor(np.abs(s) + np.float32(0.5))
    return (q * d).astype(np.float64).reshape(x.shape)


class QMat:
    """a matrix of a quantised type: its products see the input as the engine does"""

    def __init__(self, a):
        self.a = a

    def __matmul__(self, h):
        return self.a @ qdq(h)

    def __getitem__(self, i):
        return self.a[i]


def build(arch, rng):
    merges = train_bpe(160)
    tokens = [B2U[b] for b in range(256)]
    for a, b in merges:
        if a + b not in tokens:
            tokens.append(a + b)
    if arch == "llama":  # words the merges never build: only whole-word lookup finds them
        tokens += [w for w in ("\u0120quickly", "\u0120brownish") if w not in tokens]
    tokens += SPECIALS
    while len(tokens) % 32:
        tokens.append("<|extra_%d|>" % len(tokens))
    vocab = {t: i for i, t in enumerate(tokens)}
    ranks = {m: i for i, m in enumerate(merges)}
    ttype = [3 if t.startswith("<|") else 1 for t in tokens]

    dim, ffn, heads, kv_heads, layers = 256, 256, 4, 2, 2
    if arch == "gemma3":
        layers = 6  # five window layers, then one that sees everything
    hd = 64 if arch not in ("qwen3", "gemma3") else 32  # qwen3, gemma3: head_dim != dim / heads
    gemma = arch.startswith("gemma")
    nv = len(tokens)
    meta = [
        ("general.architecture", T_STR, arch),
        ("general.name", T_STR, "SynthOS tiny test model"),
        ("general.alignment", T_U32, 32),
        (arch + ".context_length", T_U32, 4096),
        (arch + ".embedding_length", T_U32, dim),
        (arch + ".block_count", T_U32, layers),
        (arch + ".feed_forward_length", T_U32, ffn),
        (arch + ".attention.head_count", T_U32, heads),
        (arch + ".attention.head_count_kv", T_U32, kv_heads),
        (arch + ".rope.freq_base", T_F32, 1000000.0 if arch != "llama" else 500000.0),
        (arch + (".attention.layer_norm_epsilon" if arch == "starcoder2" else ".attention.layer_norm_rms_epsilon"),
         T_F32, 1e-6),
        (arch + ".attention.key_length", T_U32, hd),
        (arch + ".attention.value_length", T_U32, hd),
    ] + {
        "minicpm": [(arch + ".embedding_scale", T_F32, 12.0), (arch + ".residual_scale", T_F32, 0.7),
                    (arch + ".logit_scale", T_F32, 2.0)],
        "phi3": [(arch + ".rope.dimension_count", T_U32, 32)],
        "gemma2": [(arch + ".attn_logit_softcapping", T_F32, 3.0), (arch + ".final_logit_softcapping", T_F32, 6.0),
                   (arch + ".attention.sliding_window", T_U32, 3)],
        "gemma3": [(arch + ".attention.sliding_window", T_U32, 3), (arch + ".rope.scaling.factor", T_F32, 4.0)],
    }.get(arch, []) + [
        ("tokenizer.ggml.model", T_STR, "gpt2"),
        ("tokenizer.ggml.pre", T_STR, {"llama": "minicpm5", "starcoder2": "starcoder"}.get(arch, "qwen2")),
        ("tokenizer.ggml.tokens", T_ARR, (T_STR, tokens)),
        ("tokenizer.ggml.token_type", T_ARR, (T_I32, ttype)),
        ("tokenizer.ggml.merges", T_ARR, (T_STR, [a + " " + b for a, b in merges])),
        ("tokenizer.ggml.eos_token_id", T_U32, vocab["<|im_end|>"]),
        ("tokenizer.ggml.padding_token_id", T_U32, vocab["<|endoftext|>"]),
        ("tokenizer.ggml.add_bos_token", T_BOOL, 0),
    ]
    plan = [
        {"attn_q": "Q4_K", "attn_k": "Q5_K", "attn_v": "Q6_K", "attn_output": "Q8_0", "ffn_gate": "Q3_K",
         "ffn_up": "Q2_K", "ffn_down": "Q4_0"},
        {"attn_q": "Q5_0", "attn_k": "F32", "attn_v": "F16", "attn_output": "Q4_1", "ffn_gate": "BF16",
         "ffn_up": "Q5_1", "ffn_down": "Q8_0"},
    ]
    tensors, W = [], {}

    def add(name, typ, cols, rows, scale):
        raw = make_raw(rng, typ, cols * rows, scale)
        tensors.append((name, typ, [cols] if rows == 1 else [cols, rows], raw))
        W[name] = dequant(typ, raw, cols * rows).reshape(rows, cols) if rows > 1 else dequant(typ, raw, cols)
        if rows > 1 and typ in INT_TYPES and cols % 32 == 0 and name != "token_embd.weight":
            W[name] = QMat(W[name])

    add("token_embd.weight", "F16", dim, nv, 0.1 if arch in ("minicpm", "gemma", "gemma2", "gemma3") else 1.0)
    if arch == "phi3":  # long-context rope factors (the short ones are used)
        add("rope_factors_short.weight", "F32", 16, 1, 1.0)
        W["rope_factors_short.weight"] = np.abs(W["rope_factors_short.weight"]) * 3 + 1
        tensors[-1] = (tensors[-1][0], "F32", [16], W["rope_factors_short.weight"].astype(np.float32).tobytes())
    for l in range(layers):
        p = "blk.%d." % l
        pl = plan[l % 2]
        add(p + "attn_norm.weight", "F32", dim, 1, 1.0)
        if arch == "starcoder2":
            add(p + "attn_norm.bias", "F32", dim, 1, 0.2)
        if arch == "phi3":
            add(p + "attn_qkv.weight", pl["attn_q"], dim, (heads + 2 * kv_heads) * hd, 0.08)
            qkv = W[p + "attn_qkv.weight"]
            a = qkv.a if isinstance(qkv, QMat) else qkv
            wrap = QMat if isinstance(qkv, QMat) else (lambda x: x)
            W[p + "attn_q.weight"] = wrap(a[:heads * hd])
            W[p + "attn_k.weight"] = wrap(a[heads * hd:(heads + kv_heads) * hd])
            W[p + "attn_v.weight"] = wrap(a[(heads + kv_heads) * hd:])
        else:
            add(p + "attn_q.weight", pl["attn_q"], dim, heads * hd, 0.08)
            add(p + "attn_k.weight", pl["attn_k"], dim, kv_heads * hd, 0.08)
            add(p + "attn_v.weight", pl["attn_v"], dim, kv_heads * hd, 0.08)
        if arch in ("qwen2", "starcoder2"):
            add(p + "attn_q.bias", "F32", heads * hd, 1, 0.2)
            add(p + "attn_k.bias", "F32", kv_heads * hd, 1, 0.2)
            add(p + "attn_v.bias", "F32", kv_heads * hd, 1, 0.2)
        if arch in ("qwen3", "gemma3"):
            add(p + "attn_q_norm.weight", "F32", hd, 1, 1.0)
            add(p + "attn_k_norm.weight", "F32", hd, 1, 1.0)
        add(p + "attn_output.weight", pl["attn_output"], heads * hd, dim, 0.08)
        if arch in ("gemma2", "gemma3"):
            add(p + "post_attention_norm.weight", "F32", dim, 1, 1.0)
        if arch == "starcoder2":
            add(p + "attn_output.bias", "F32", dim, 1, 0.2)
        add(p + "ffn_norm.weight", "F32", dim, 1, 1.0)
        if arch == "starcoder2":
            add(p + "ffn_norm.bias", "F32", dim, 1, 0.2)
        elif arch != "phi3":
            add(p + "ffn_gate.weight", pl["ffn_gate"], dim, ffn, 0.08)
        add(p + "ffn_up.weight", pl["ffn_up"], dim, ffn * (2 if arch == "phi3" else 1), 0.08)
        add(p + "ffn_down.weight", pl["ffn_down"], ffn, dim, 0.06)
        if arch in ("gemma2", "gemma3"):
            add(p + "post_ffw_norm.weight", "F32", dim, 1, 1.0)
        if arch == "starcoder2":
            add(p + "ffn_up.bias", "F32", ffn, 1, 0.2)
            add(p + "ffn_down.bias", "F32", dim, 1, 0.2)
    add("output_norm.weight", "F32", dim, 1, 1.0)
    if arch == "starcoder2":
        add("output_norm.bias", "F32", dim, 1, 0.2)
    if not gemma:  # Gemma ties the output to the embeddings
        add("output.weight", "Q6_K", dim, nv, 0.08)
    cfg = dict(arch=arch, dim=dim, ffn=ffn, heads=heads, kv=kv_heads, hd=hd, layers=layers, eps=1e-6,
               base=1000000.0 if arch != "llama" else 500000.0, rot=32 if arch == "phi3" else hd,
               emb=dict(minicpm=12.0).get(arch, math.sqrt(dim) if gemma else 1.0),
               res=0.7 if arch == "minicpm" else 1.0, logit_div=2.0 if arch == "minicpm" else 1.0,
               att_cap=3.0 if arch == "gemma2" else 0, final_cap=6.0 if arch == "gemma2" else 0,
               swa=3 if arch in ("gemma2", "gemma3") else 0, pattern=dict(gemma2=2, gemma3=6).get(arch, 0))
    if gemma:
        W["output.weight"] = W["token_embd.weight"]
    return meta, tensors, W, cfg, vocab, ranks


def rmsnorm(x, w, eps):
    return x / np.sqrt(np.mean(x * x) + eps) * w


def rope(v, pos, hd, base, neox, factors=None, scale=1.0):
    """hd: the dimensions that rotate (the rest stay)"""
    out = v.copy()
    for i in range(hd // 2):
        theta = pos * np.float32(base ** (-2.0 * i / hd)) / (factors[i] if factors is not None else 1.0) / scale
        c, s = math.cos(theta), math.sin(theta)
        a, b = (i, i + hd // 2) if neox else (2 * i, 2 * i + 1)
        out[a] = v[a] * c - v[b] * s
        out[b] = v[a] * s + v[b] * c
    return out


def norm(W, name, x, cfg):
    if cfg["arch"] != "starcoder2":
        return rmsnorm(x, W[name + ".weight"], cfg["eps"])
    m = x.mean()
    return (x - m) / np.sqrt(((x - m) ** 2).mean() + cfg["eps"]) * W[name + ".weight"] + W[name + ".bias"]


def forward(W, cfg, tokens):
    """logits after every position"""
    hd, H, KV = cfg["hd"], cfg["heads"], cfg["kv"]
    arch = cfg["arch"]
    neox = arch not in ("llama", "minicpm")
    f16 = lambda a: np.asarray(a, dtype=np.float32).astype(np.float16).astype(np.float64)  # the engine's cache
    kc = [[] for _ in range(cfg["layers"])]
    vc = [[] for _ in range(cfg["layers"])]
    res = []
    for pos, t in enumerate(tokens):
        x = W["token_embd.weight"][t].copy() * cfg["emb"]
        for l in range(cfg["layers"]):
            window = cfg["swa"] if cfg["swa"] and l % cfg["pattern"] < cfg["pattern"] - 1 else 0
            base, scale = cfg["base"], 1.0
            if arch == "gemma3":
                base, scale = (10000.0, 1.0) if window else (cfg["base"], 4.0)
            factors = W.get("rope_factors_short.weight")
            p = "blk.%d." % l
            h = norm(W, p + "attn_norm", x, cfg)
            q = W[p + "attn_q.weight"] @ h
            k = W[p + "attn_k.weight"] @ h
            v = W[p + "attn_v.weight"] @ h
            if cfg["arch"] in ("qwen2", "starcoder2"):
                q += W[p + "attn_q.bias"]
                k += W[p + "attn_k.bias"]
                v += W[p + "attn_v.bias"]
            q = q.reshape(H, hd)
            k = k.reshape(KV, hd)
            if arch in ("qwen3", "gemma3"):
                q = np.array([rmsnorm(r, W[p + "attn_q_norm.weight"], cfg["eps"]) for r in q])
                k = np.array([rmsnorm(r, W[p + "attn_k_norm.weight"], cfg["eps"]) for r in k])
            q = np.array([rope(r, pos, cfg["rot"], base, neox, factors, scale) for r in q])
            k = np.array([rope(r, pos, cfg["rot"], base, neox, factors, scale) for r in k])
            kc[l].append(f16(k))
            vc[l].append(f16(v.reshape(KV, hd)))
            first = max(0, len(kc[l]) - window) if window else 0
            att = np.zeros(H * hd)
            for hh in range(H):
                g = hh // (H // KV)
                sc = np.array([q[hh] @ kk[g] for kk in kc[l][first:]]) / math.sqrt(hd)
                if cfg["att_cap"]:
                    sc = cfg["att_cap"] * np.tanh(sc / cfg["att_cap"])
                sc = np.exp(sc - sc.max())
                sc /= sc.sum()
                att[hh * hd:(hh + 1) * hd] = sum(w * vv[g] for w, vv in zip(sc, vc[l][first:]))
            o = W[p + "attn_output.weight"] @ att
            if arch == "starcoder2":
                o = o + W[p + "attn_output.bias"]
            if arch in ("gemma2", "gemma3"):
                o = rmsnorm(o, W[p + "post_attention_norm.weight"], cfg["eps"])
            x = x + cfg["res"] * o
            h = norm(W, p + "ffn_norm", x, cfg)
            if arch == "starcoder2":
                o = W[p + "ffn_down.weight"] @ gelu(W[p + "ffn_up.weight"] @ h + W[p + "ffn_up.bias"])
                o = o + W[p + "ffn_down.bias"]
            else:
                if arch == "phi3":
                    both = W[p + "ffn_up.weight"] @ h
                    gt, up = both[:cfg["ffn"]], both[cfg["ffn"]:]
                else:
                    gt = W[p + "ffn_gate.weight"] @ h
                    up = W[p + "ffn_up.weight"] @ h
                act = gelu(gt) if arch.startswith("gemma") else gt / (1 + np.exp(-gt))
                o = W[p + "ffn_down.weight"] @ (act * up)
            if arch in ("gemma2", "gemma3"):
                o = rmsnorm(o, W[p + "post_ffw_norm.weight"], cfg["eps"])
            x = x + cfg["res"] * o
        x = norm(W, "output_norm", x, cfg)
        lg = (W["output.weight"] @ x) / cfg["logit_div"]
        if cfg["final_cap"]:
            lg = cfg["final_cap"] * np.tanh(lg / cfg["final_cap"])
        res.append(lg)
    return res


def greedy(W, cfg, prompt, n):
    ids = list(prompt)
    margins = []
    for _ in range(n):
        logits = forward(W, cfg, ids)[-1]
        order = np.argsort(-logits)
        margins.append(logits[order[0]] - logits[order[1]])
        ids.append(int(order[0]))
    return ids[len(prompt):], min(margins)


def complete_text(W, cfg, vocab, tk, before):
    """what ai complete prints for before (plain text): the last word is healed, greedy after that; with the
    smallest gap between the best token and the next allowed one"""
    inv = {i: t for t, i in vocab.items()}
    u2b = {c: b for b, c in B2U.items()}

    def piece(i):
        t = inv[i]
        return b"" if t.startswith("<|") else bytes(u2b[c] for c in t)

    def word(c):
        return chr(c).isalnum() or c == ord("_") or c >= 0x80

    b = before.encode()
    n = cut = len(b)
    while cut > 0 and n - cut < 40 and word(b[cut - 1]):
        cut -= 1
    if cut > 0 and b[cut - 1] == ord(" ") and n - cut < 40:
        cut -= 1
    heal, text = b[cut:], b[max(0, cut - 400):cut].decode()
    ids = tk(text if text else "\n")
    out, made, gaps = b"", 0, []
    while True:
        logits = forward(W, cfg, ids)[-1]
        allowed = []
        for i in range(len(logits)):
            p = piece(i)
            if heal and (not p or p[:len(heal)] != heal[:len(p)]):
                continue
            allowed.append(i)
        order = sorted(allowed, key=lambda i: -logits[i])
        if not order:
            return None, 0
        best = order[0]
        if len(order) > 1:
            gaps.append(logits[best] - logits[order[1]])
        p = piece(best)
        if not p:
            break
        if heal:
            used = min(len(p), len(heal))
            heal, p = heal[used:], p[used:]
        nl = b"\n" in p
        p = p.split(b"\n")[0]
        out += p
        made += 1
        sentence = out and not heal and out[-1:] in (b".", b"!", b"?")
        if nl or sentence or made >= 12 or len(out) >= 96:
            break
        ids.append(best)
    return (None if heal else out), (min(gaps) if gaps else 9)


def make(out, arch, fname, with_tokens):
    """writes OUT/<fname>.gguf; returns its run.sh cases"""
    seed = 1
    while True:
        rng = np.random.default_rng(seed)
        meta, tensors, W, cfg, vocab, ranks = build(arch, rng)
        tk = (lambda text: tokenize(text, vocab, ranks, 3, True)) if arch == "llama" else \
            (lambda text: tokenize(text, vocab, ranks))
        prompt = tk("<|im_start|>user\nhello world<|im_end|>\n<|im_start|>assistant\n")
        gen, margin = greedy(W, cfg, prompt, 6)
        if margin > 0.02:  # float32 in the engine must not flip a choice
            break
        seed += 1
    path = "/mnt/hdb/%s.gguf" % fname
    write_gguf("%s/%s.gguf" % (out, fname), meta, tensors)
    cases = ["ai load " + path, "loaded SynthOS tiny test model (%s)" % path]
    if with_tokens:
        for text in ["hello world", "the quick brown fox jumps", "it's   done ok?! 2024"]:
            cases += ['ai tokens "%s"' % text, "tokens: " + " ".join(map(str, tk(text)))]
        sp = "<|im_start|>user hi<|im_end|>"
        cases += ['ai tokens -s "%s"' % sp, "tokens: " + " ".join(map(str, tk(sp)))]
    if arch == "llama":
        text = "a quickly brownish fox 12345"
        cases += ['ai tokens "%s"' % text, "tokens: " + " ".join(map(str, tk(text)))]
    cases += ["ai next %s -n 6" % " ".join(map(str, prompt)), "next: " + " ".join(map(str, gen))]
    if with_tokens:  # Tab completion with this model: the first prompt that completes to plain text, safely
        for before in ["hello wor", "the quick bro", "it is do", "the brown fox ju", "x", "t", "a"]:
            out, gap = complete_text(W, cfg, vocab, tk, before)
            if out and gap > 0.02 and len(out.strip()) >= 2:
                shown = "".join(chr(c) if 32 <= c < 127 and c != 92 else "\\x%02x" % c for c in out)
                cases += ["ai complete --model %s %s" % (path, before), "completion: [%s]" % shown]
                break
        else:
            raise SystemExit("no prompt completes cleanly")
    return cases



# ---- a tiny Whisper (speech to text) in the whisper.cpp file format, and what it must hear ----

WH = dict(n_vocab=300 + 1608, a_ctx=24, dim=64, heads=2, a_layers=2, t_ctx=16, t_layers=2, n_mels=80)


def wh_build(rng):
    """the tensors (name, type, dims as ggml lists them, raw) and the same values as float arrays"""
    D, V, A, T, M = WH["dim"], WH["n_vocab"], WH["a_ctx"], WH["t_ctx"], WH["n_mels"]
    tensors, W = [], {}
    kinds = ["F16", "Q8_0", "F32", "Q8_0"]

    def add(name, typ, shape, scale):
        n = int(np.prod(shape))
        raw = make_raw(rng, typ, n, scale)
        tensors.append((name, typ, list(reversed(shape)), raw))
        W[name] = dequant(typ, raw, n).reshape(shape)
        WT[name] = typ

    add("encoder.positional_embedding", "F32", (A, D), 0.5)
    add("encoder.conv1.weight", "F16", (D, M, 3), 0.15)
    add("encoder.conv1.bias", "F32", (D, 1), 0.1)
    add("encoder.conv2.weight", "F16", (D, D, 3), 0.1)
    add("encoder.conv2.bias", "F32", (D, 1), 0.1)
    for i in range(WH["a_layers"]):
        p = "encoder.blocks.%d." % i
        block(add, p, D, kinds[i % 4], False)
    add("encoder.ln_post.weight", "F32", (D,), 1.0)
    add("encoder.ln_post.bias", "F32", (D,), 0.1)
    add("decoder.positional_embedding", "F32", (T, D), 3.0)
    add("decoder.token_embedding.weight", "F16", (V, D), 1.0)
    for i in range(WH["t_layers"]):
        p = "decoder.blocks.%d." % i
        block(add, p, D, kinds[(i + 1) % 4], True)
    add("decoder.ln.weight", "F32", (D,), 1.0)
    add("decoder.ln.bias", "F32", (D,), 0.1)
    for k in list(W):
        if k.endswith(("ln.weight", "ln_post.weight")):
            W[k] = W[k] * 0.2 + 1.0
    # the norm weights near 1 also in the file: rewrite them
    for idx, (name, typ, dims, raw) in enumerate(tensors):
        if name.endswith(("ln.weight", "ln_post.weight")):
            tensors[idx] = (name, typ, dims, W[name].astype("<f4").tobytes())
    return tensors, W


def block(add, p, D, kind, cross):
    add(p + "attn_ln.weight", "F32", (D,), 1.0)
    add(p + "attn_ln.bias", "F32", (D,), 0.1)
    for part in (["attn"] + (["cross_attn"] if cross else [])):
        if part == "cross_attn":
            add(p + "cross_attn_ln.weight", "F32", (D,), 1.0)
            add(p + "cross_attn_ln.bias", "F32", (D,), 0.1)
        add(p + part + ".query.weight", kind, (D, D), 0.15)
        add(p + part + ".query.bias", "F32", (D,), 0.1)
        add(p + part + ".key.weight", kind, (D, D), 0.15)
        add(p + part + ".value.weight", kind, (D, D), 0.15)
        add(p + part + ".value.bias", "F32", (D,), 0.1)
        add(p + part + ".out.weight", kind, (D, D), 0.15)
        add(p + part + ".out.bias", "F32", (D,), 0.1)
    add(p + "mlp_ln.weight", "F32", (D,), 1.0)
    add(p + "mlp_ln.bias", "F32", (D,), 0.1)
    add(p + "mlp.0.weight", kind, (4 * D, D), 0.12)
    add(p + "mlp.0.bias", "F32", (4 * D,), 0.1)
    add(p + "mlp.2.weight", kind, (D, 4 * D), 0.06)
    add(p + "mlp.2.bias", "F32", (D,), 0.1)


def wh_words():
    words = [bytes([b]) for b in range(256)]
    for a in "etaoinsh":
        for b in "rdlu c":
            if len(words) < 300:
                words.append((" " + a + b).encode() if b != " " else (a + "s").encode())
    while len(words) < 300:
        words.append(b"x%d" % len(words))
    return words[:300]


def wh_write(path, tensors, filters, words):
    out = struct.pack("<12i", 0x67676d6c, WH["n_vocab"], WH["a_ctx"], WH["dim"], WH["heads"], WH["a_layers"],
                      WH["t_ctx"], WH["dim"], WH["heads"], WH["t_layers"], WH["n_mels"], 1)
    out += struct.pack("<2i", WH["n_mels"], 201) + filters.astype("<f4").tobytes()
    out += struct.pack("<i", len(words)) + b"".join(struct.pack("<I", len(w)) + w for w in words)
    for name, typ, dims, raw in tensors:
        nb = name.encode()
        out += struct.pack("<3i", len(dims), len(nb), GGML[typ]) + struct.pack("<%di" % len(dims), *dims) + nb + raw
    open(path, "wb").write(out)


def wh_mel(pcm, filters):
    frames = 2 * WH["a_ctx"]
    n = np.arange(400)
    window = 0.5 - 0.5 * np.cos(2 * np.pi * n / 400)
    mel = np.zeros((WH["n_mels"], frames))
    k = np.arange(201)[:, None] * n[None, :]
    cos, sin = np.cos(2 * np.pi * k / 400), np.sin(2 * np.pi * k / 400)
    for t in range(frames):
        at = t * 160 - 200 + n
        at = np.abs(at)
        x = np.where(at < len(pcm), pcm[np.minimum(at, len(pcm) - 1)], 0.0) * window
        power = (cos @ x) ** 2 + (sin @ x) ** 2
        mel[:, t] = filters @ power
    mel = np.log10(np.maximum(mel, 1e-10))
    mel = np.maximum(mel, mel.max() - 8.0)
    return (mel + 4.0) / 4.0


def ln(x, g, b):
    m = x.mean(-1, keepdims=True)
    v = ((x - m) ** 2).mean(-1, keepdims=True)
    return (x - m) / np.sqrt(v + 1e-5) * g + b


def gelu(x):
    return 0.5 * x * (1 + np.tanh(0.7978845608 * (x + 0.044715 * x ** 3)))


def conv(x, w, b, stride):
    cin, L = x.shape
    pad = np.concatenate([np.zeros((cin, 1)), x, np.zeros((cin, 1))], axis=1)
    lout = L // stride
    out = np.zeros((w.shape[0], lout))
    for t in range(lout):
        seg = pad[:, t * stride:t * stride + 3]
        out[:, t] = np.einsum("oik,ik->o", w, seg)
    return gelu(out + b.reshape(-1, 1))


def attend(q, k, v, heads, causal_from=None):
    D = q.shape[1]
    hd = D // heads
    out = np.zeros_like(q)
    for h in range(heads):
        s = q[:, h * hd:(h + 1) * hd] @ k[:, h * hd:(h + 1) * hd].T / math.sqrt(hd)
        if causal_from is not None:
            for i in range(s.shape[0]):
                s[i, causal_from + i + 1:] = -1e30
        s = np.exp(s - s.max(1, keepdims=True))
        s /= s.sum(1, keepdims=True)
        out[:, h * hd:(h + 1) * hd] = s @ v[:, h * hd:(h + 1) * hd]
    return out


WT = {}  # the whisper tensors' types


def lin(W, name, x, bias=True):
    if WT.get(name + ".weight") in INT_TYPES and x.shape[-1] % 32 == 0:
        x = qdq(x)
    y = x @ W[name + ".weight"].T
    return y + W[name + ".bias"] if bias else y


def wh_encode(W, mel):
    x = conv(mel, W["encoder.conv1.weight"], W["encoder.conv1.bias"], 1)
    x = conv(x, W["encoder.conv2.weight"], W["encoder.conv2.bias"], 2).T + W["encoder.positional_embedding"]
    for i in range(WH["a_layers"]):
        p = "encoder.blocks.%d." % i
        h = ln(x, W[p + "attn_ln.weight"], W[p + "attn_ln.bias"])
        a = attend(lin(W, p + "attn.query", h), lin(W, p + "attn.key", h, False), lin(W, p + "attn.value", h),
                   WH["heads"])
        x = x + lin(W, p + "attn.out", a)
        h = ln(x, W[p + "mlp_ln.weight"], W[p + "mlp_ln.bias"])
        x = x + lin(W, p + "mlp.2", gelu(lin(W, p + "mlp.0", h)))
    return ln(x, W["encoder.ln_post.weight"], W["encoder.ln_post.bias"])


def wh_decode(W, enc, tokens):
    """the logits after the last of tokens"""
    x = W["decoder.token_embedding.weight"][tokens] + W["decoder.positional_embedding"][:len(tokens)]
    for i in range(WH["t_layers"]):
        p = "decoder.blocks.%d." % i
        h = ln(x, W[p + "attn_ln.weight"], W[p + "attn_ln.bias"])
        a = attend(lin(W, p + "attn.query", h), lin(W, p + "attn.key", h, False), lin(W, p + "attn.value", h),
                   WH["heads"], causal_from=0)
        x = x + lin(W, p + "attn.out", a)
        h = ln(x, W[p + "cross_attn_ln.weight"], W[p + "cross_attn_ln.bias"])
        a = attend(lin(W, p + "cross_attn.query", h), lin(W, p + "cross_attn.key", enc, False),
                   lin(W, p + "cross_attn.value", enc), WH["heads"])
        x = x + lin(W, p + "cross_attn.out", a)
        h = ln(x, W[p + "mlp_ln.weight"], W[p + "mlp_ln.bias"])
        x = x + lin(W, p + "mlp.2", gelu(lin(W, p + "mlp.0", h)))
    x = ln(x, W["decoder.ln.weight"], W["decoder.ln.bias"])
    return x[-1] @ W["decoder.token_embedding.weight"].T


def pick(logits, lo, hi, margins):
    seg = logits[lo:hi]
    order = np.argsort(-seg)
    margins.append(seg[order[0]] - seg[order[1]])
    return lo + int(order[0])


def wh_transcribe(W, pcm, filters):
    V = WH["n_vocab"]
    eot = V - 1509 - 99
    sot, langs = eot + 1, 99
    transcribe, no_ts = sot + 1 + langs + 1, sot + 1 + langs + 5
    enc = wh_encode(W, wh_mel(pcm, filters))
    margins = []
    lang = pick(wh_decode(W, enc, [sot]), sot + 1, sot + 1 + langs, margins) - (sot + 1)
    toks = [sot, sot + 1 + lang, transcribe, no_ts]
    out = []
    while len(toks) < WH["t_ctx"]:
        logits = wh_decode(W, enc, toks)
        best = pick(logits, 0, eot + 1, margins) if out else pick(logits, 0, eot, margins)
        if best == eot:
            break
        out.append(best)
        toks.append(best)
    return lang, out, min(margins)


def write_wav(path, pcm):
    data = np.round(pcm * 32768).astype("<i2").tobytes()
    head = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
    open(path, "wb").write(head + b"data" + struct.pack("<I", len(data)) + data)


def make_whisper(out):
    seed = 1
    while True:
        rng = np.random.default_rng(1000 + seed)
        tensors, W = wh_build(rng)
        filters = rng.uniform(0, 0.02, (WH["n_mels"], 201)).astype(np.float32).astype(np.float64)
        t = np.arange(6000) / 16000
        pcm = 0.3 * np.sin(2 * np.pi * 440 * t) * np.sin(np.pi * t / 0.375) + 0.1 * rng.standard_normal(6000)
        pcm = (np.clip(pcm, -1, 1) * 32767).astype(np.int16).astype(np.float64) / 32768  # what the WAV file holds
        lang, toks, margin = wh_transcribe(W, pcm, filters)
        if margin > 0.02 and len(set(toks)) >= 3:
            break
        seed += 1
    wh_write(out + "/whisper.bin", tensors, filters.astype(np.float32), wh_words())
    write_wav(out + "/speech.wav", pcm)
    return ["ai hear /mnt/hdb/speech.wav --model /mnt/hdb/whisper.bin",
            "heard tokens: " + " ".join(map(str, toks)) + " (language %d)" % lang,
            # streaming: attempts every 500 ms, then the whole of it, which must be the same words
            "ai hear --stream /mnt/hdb/speech.wav --model /mnt/hdb/whisper.bin",
            "streamed tokens: " + " ".join(map(str, toks)) + " (language %d)" % lang]


# ---- a tiny Qwen3-TTS: the talker, the code predictor and the codec, and what it must say ----

TT = dict(dim=64, ffn=128, heads=4, kv=2, hd=16, layers=2, text_dim=48, proj=96, groups=4, codes=64, vocab=128,
          cp_layers=1, eps=1e-6, theta=1000000.0)
TC = dict(cb_dim=16, hidden=32, latent=64, dec=48, heads=2, hd=16, ffn=64, layers=2, window=4, eps=1e-5,
          theta=10000.0)
TID = dict(pad=100, bos=101, eos=102, think=104, nothink=105, think_bos=106, think_eos=107)
TLANG = {"english": 110, "russian": 111}
TSPK = {"alice": 120, "bob": 121}


class TTSBuilder:
    def __init__(self, rng):
        self.rng, self.tensors, self.W = rng, [], {}

    def add(self, name, typ, dims, scale, offset=0.0):
        """dims as ggml lists them (fastest first); W holds the numpy array with the dimensions reversed"""
        n = int(np.prod(dims))
        raw = make_raw(self.rng, typ, n, scale)
        if offset:
            raw = (dequant(typ, raw, n) + offset).astype("<f4").tobytes()
        self.tensors.append((name, typ, list(dims), raw))
        a = dequant(typ, raw, n).reshape(tuple(reversed(dims)))
        self.W[name] = QMat(a) if typ in INT_TYPES and len(dims) == 2 else a


def tts_qwen_build(b, pre, layers):
    D, F, H, KV, hd = TT["dim"], TT["ffn"], TT["heads"], TT["kv"], TT["hd"]
    for l in range(layers):
        p = "%s.blk.%d." % (pre, l)
        b.add(p + "attn_norm.weight", "F32", [D], 0.1, 1.0)
        b.add(p + "ffn_norm.weight", "F32", [D], 0.1, 1.0)
        b.add(p + "attn_q.weight", "Q8_0" if l == 0 else "F16", [D, H * hd], 0.12)
        b.add(p + "attn_k.weight", "F16", [D, KV * hd], 0.12)
        b.add(p + "attn_v.weight", "F32", [D, KV * hd], 0.12)
        b.add(p + "attn_output.weight", "F16", [H * hd, D], 0.12)
        b.add(p + "attn_q_norm.weight", "F32", [hd], 0.1, 1.0)
        b.add(p + "attn_k_norm.weight", "F32", [hd], 0.1, 1.0)
        b.add(p + "ffn_gate.weight", "F16", [D, F], 0.12)
        b.add(p + "ffn_up.weight", "F16", [D, F], 0.12)
        b.add(p + "ffn_down.weight", "F16", [F, D], 0.1)
    b.add(pre + ".output_norm.weight", "F32", [D], 0.1, 1.0)


def tts_lin(W, name, x):
    w = W[name]
    return w @ x if isinstance(w, QMat) else w @ x


class TTSStack:
    """the talker's or the code predictor's Qwen3 layers, fed vectors, with a cache"""

    def __init__(self, W, pre, layers):
        self.W, self.pre, self.layers = W, pre, layers
        self.reset()

    def reset(self):
        self.kc = [[] for _ in range(self.layers)]
        self.vc = [[] for _ in range(self.layers)]

    def step(self, x):
        """one input vector; the normalised state after it"""
        W, hd, H, KV, eps = self.W, TT["hd"], TT["heads"], TT["kv"], TT["eps"]
        pos = len(self.kc[0])
        for l in range(self.layers):
            p = "%s.blk.%d." % (self.pre, l)
            h = rmsnorm(x, W[p + "attn_norm.weight"], eps)
            q = tts_lin(W, p + "attn_q.weight", h).reshape(H, hd)
            k = tts_lin(W, p + "attn_k.weight", h).reshape(KV, hd)
            v = tts_lin(W, p + "attn_v.weight", h).reshape(KV, hd)
            q = np.array([rope(rmsnorm(r, W[p + "attn_q_norm.weight"], eps), pos, hd, TT["theta"], True) for r in q])
            k = np.array([rope(rmsnorm(r, W[p + "attn_k_norm.weight"], eps), pos, hd, TT["theta"], True) for r in k])
            self.kc[l].append(k)
            self.vc[l].append(v)
            att = np.zeros(H * hd)
            for a in range(H):
                g = a // (H // KV)
                sc = np.array([q[a] @ kk[g] for kk in self.kc[l]]) / math.sqrt(hd)
                sc = np.exp(sc - sc.max())
                sc /= sc.sum()
                att[a * hd:(a + 1) * hd] = sum(w * vv[g] for w, vv in zip(sc, self.vc[l]))
            x = x + tts_lin(W, p + "attn_output.weight", att)
            h = rmsnorm(x, W[p + "ffn_norm.weight"], eps)
            gt = tts_lin(W, p + "ffn_gate.weight", h)
            x = x + tts_lin(W, p + "ffn_down.weight", gt / (1 + np.exp(-gt)) * tts_lin(W, p + "ffn_up.weight", h))
        return rmsnorm(x, W[self.pre + ".output_norm.weight"], eps)


def tts_text(W, i):
    h = W["talker.text_proj.fc1.weight"] @ W["talker.text_embd.weight"][i] + W["talker.text_proj.fc1.bias"]
    return W["talker.text_proj.fc2.weight"] @ (h / (1 + np.exp(-h))) + W["talker.text_proj.fc2.bias"]


def tts_codes(W, role, body, lang, speaker, frames):
    """the codes greedy decoding makes, and the smallest gap between a choice and the next"""
    nt = W["talker.text_embd.weight"].shape[0]
    pad_t, bos_t, eos_t = (tts_text(W, nt - 3 + i) for i in range(3))
    ce = W["talker.codec_embd.weight"]
    left = [TID["think"], TID["think_bos"], TLANG[lang], TID["think_eos"], TSPK[speaker], TID["pad"]]
    rows = [tts_text(W, i) for i in role]
    rows += [ce[c] + (bos_t if i == len(left) - 1 else pad_t) for i, c in enumerate(left)]
    rows += [tts_text(W, i) + ce[TID["pad"]] for i in body] + [eos_t + ce[TID["pad"]], pad_t + ce[TID["bos"]]]
    talker, cp = TTSStack(W, "talker", TT["layers"]), TTSStack(W, "code_pred", TT["cp_layers"])
    for r in rows:
        h = talker.step(r)
    out, hist, gaps = [], [], []
    for _ in range(frames):
        lg = W["talker.codec_head.weight"] @ h
        for t in set(hist):
            lg[t] = lg[t] * 1.05 if lg[t] < 0 else lg[t] / 1.05
        lg[TT["codes"]:] = np.where(np.arange(TT["codes"], TT["vocab"]) == TID["eos"], lg[TT["codes"]:], -1e30)
        order = np.argsort(-lg)
        gaps.append(lg[order[0]] - lg[order[1]])
        c0 = int(order[0])
        if c0 == TID["eos"]:
            break
        hist.append(c0)
        codes = [c0]
        cp.reset()
        cp.step(h)
        hc = cp.step(ce[c0])
        for g in range(TT["groups"] - 1):
            if g:
                hc = cp.step(W["code_pred.codec_embd.%d.weight" % (g - 1)][codes[g]])
            lg = W["code_pred.lm_head.%d.weight" % g] @ hc
            order = np.argsort(-lg)
            gaps.append(lg[order[0]] - lg[order[1]])
            codes.append(int(order[0]))
        out.append(codes)
        x = ce[c0] + sum(W["code_pred.codec_embd.%d.weight" % i][codes[i + 1]] for i in range(TT["groups"] - 1))
        h = talker.step(x + pad_t)
    return out, min(gaps)


def tts_conv(x, w, b, dil=1):
    """causal; x [T][cin], w [cout][cin][k]"""
    K, T = w.shape[2], x.shape[0]
    xp = np.concatenate([np.zeros(((K - 1) * dil, x.shape[1])), x])
    return sum(xp[k * dil:k * dil + T] @ w[:, :, k].T for k in range(K)) + b


def tts_tconv(x, w, b, s):
    """causal transposed; w [cin][cout][k]"""
    T, K = x.shape[0], w.shape[2]
    y = np.zeros(((T - 1) * s + K, w.shape[1]))
    for k in range(K):
        y[k:k + (T - 1) * s + 1:s] += x @ w[:, :, k]
    return y[:T * s] + b


def tts_snake(x, W, name):
    a, ib = np.exp(W[name + ".alpha"]), 1 / (np.exp(W[name + ".beta"]) + 1e-9)
    return x + np.sin(a * x) ** 2 * ib


def tts_decode(W, codes):
    codes = np.array(codes)
    T, C = codes.shape[0], TC
    s1 = W["tok_dec.vq_first.0.codebook"][codes[:, 0]]
    s2 = sum(W["tok_dec.vq_rest.%d.codebook" % k][codes[:, k + 1]] for k in range(TT["groups"] - 1))
    x = s1 @ W["tok_dec.vq_first.output_proj.weight"][:, :, 0].T + s2 @ W["tok_dec.vq_rest.output_proj.weight"][:, :, 0].T
    x = tts_conv(x, W["tok_dec.pre_conv.weight"], W["tok_dec.pre_conv.bias"])
    P = "tok_dec.pre_tfm."
    z = np.array([tts_lin(W, P + "input_proj.weight", r) for r in x]) + W[P + "input_proj.bias"]
    H, hd = C["heads"], C["hd"]
    for l in range(C["layers"]):
        p = P + "blk.%d." % l
        h = np.array([rmsnorm(r, W[p + "attn_norm.weight"], C["eps"]) for r in z])
        q = np.array([np.array([rope(v, t, hd, C["theta"], True) for v in tts_lin(W, p + "attn_q.weight", r).reshape(H, hd)])
                      for t, r in enumerate(h)])
        k = np.array([np.array([rope(v, t, hd, C["theta"], True) for v in tts_lin(W, p + "attn_k.weight", r).reshape(H, hd)])
                      for t, r in enumerate(h)])
        v = np.array([tts_lin(W, p + "attn_v.weight", r).reshape(H, hd) for r in h])
        att = np.zeros((T, H * hd))
        for t in range(T):
            lo = max(0, t - C["window"] + 1)
            for a in range(H):
                sc = k[lo:t + 1, a] @ q[t, a] / math.sqrt(hd)
                sc = np.exp(sc - sc.max())
                sc /= sc.sum()
                att[t, a * hd:(a + 1) * hd] = sc @ v[lo:t + 1, a]
        z = z + np.array([tts_lin(W, p + "attn_output.weight", r) for r in att]) * W[p + "attn_scale"]
        h = np.array([rmsnorm(r, W[p + "ffn_norm.weight"], C["eps"]) for r in z])
        f = []
        for r in h:
            gt = tts_lin(W, p + "ffn_gate.weight", r)
            f.append(tts_lin(W, p + "ffn_down.weight", gt / (1 + np.exp(-gt)) * tts_lin(W, p + "ffn_up.weight", r)))
        z = z + np.array(f) * W[p + "ffn_scale"]
    z = np.array([rmsnorm(r, W[P + "norm.weight"], C["eps"]) for r in z])
    x = np.array([tts_lin(W, P + "output_proj.weight", r) for r in z]) + W[P + "output_proj.bias"]
    erf = np.vectorize(math.erf)
    for i in range(2):
        U = "tok_dec.upsample.%d." % i
        x = tts_tconv(x, W[U + "conv.weight"], W[U + "conv.bias"], 2)
        dw = W[U + "dwconv.weight"][:, 0, :]
        xp = np.concatenate([np.zeros((6, x.shape[1])), x])
        y = sum(xp[j:j + x.shape[0]] * dw[:, j] for j in range(7)) + W[U + "dwconv.bias"]
        m = y.mean(1, keepdims=True)
        y = (y - m) / np.sqrt(((y - m) ** 2).mean(1, keepdims=True) + 1e-6) * W[U + "norm.weight"] + W[U + "norm.bias"]
        y = np.array([tts_lin(W, U + "pwconv1.weight", r) for r in y]) + W[U + "pwconv1.bias"]
        y = 0.5 * y * (1 + erf(y / math.sqrt(2)))
        y = np.array([tts_lin(W, U + "pwconv2.weight", r) for r in y]) + W[U + "pwconv2.bias"]
        x = x + y * W[U + "gamma"]
    x = tts_conv(x, W["tok_dec.dec.0.conv.weight"], W["tok_dec.dec.0.conv.bias"])
    for i, s in enumerate((8, 5, 4, 3)):
        B = "tok_dec.dec.%d." % (i + 1)
        x = tts_tconv(tts_snake(x, W, B + "snake"), W[B + "conv_t.weight"], W[B + "conv_t.bias"], s)
        for r, d in enumerate((1, 3, 9)):
            R = B + "res.%d." % r
            y = tts_conv(tts_snake(x, W, R + "act1"), W[R + "conv1.weight"], W[R + "conv1.bias"], d)
            x = x + tts_conv(tts_snake(y, W, R + "act2"), W[R + "conv2.weight"], W[R + "conv2.bias"])
    x = tts_conv(tts_snake(x, W, "tok_dec.dec.5.snake"), W["tok_dec.dec.6.conv.weight"], W["tok_dec.dec.6.conv.bias"])
    return np.clip(x[:, 0], -1, 1)


def tts_build(rng):
    """the talker's and the codec's files: (meta, tensors) each, and all values in W"""
    meta, _, _, _, vocab, ranks = build("qwen3", rng)
    tok_meta = [m for m in meta if m[0].startswith("tokenizer.")]
    nt = len(vocab) + 32  # the text embedding has rows past the tokenizer's; the last three are tts_pad/bos/eos
    b = TTSBuilder(rng)
    D = TT["dim"]
    b.add("talker.text_embd.weight", "F16", [TT["text_dim"], nt], 1.0)
    b.add("talker.text_proj.fc1.weight", "F16", [TT["text_dim"], TT["proj"]], 0.15)
    b.add("talker.text_proj.fc1.bias", "F32", [TT["proj"]], 0.1)
    b.add("talker.text_proj.fc2.weight", "F32", [TT["proj"], D], 0.15)
    b.add("talker.text_proj.fc2.bias", "F32", [D], 0.1)
    b.add("talker.codec_embd.weight", "F16", [D, TT["vocab"]], 1.0)
    b.add("talker.codec_head.weight", "Q8_0", [D, TT["vocab"]], 0.15)
    tts_qwen_build(b, "talker", TT["layers"])
    tts_qwen_build(b, "code_pred", TT["cp_layers"])
    for g in range(TT["groups"] - 1):
        b.add("code_pred.codec_embd.%d.weight" % g, "F16", [D, TT["codes"]], 1.0)
        b.add("code_pred.lm_head.%d.weight" % g, "F32", [D, TT["codes"]], 0.15)
    tmeta = [("general.architecture", T_STR, "qwen3-tts"), ("general.name", T_STR, "SynthOS tiny test voice"),
             ("general.alignment", T_U32, 32), ("qwen3-tts.num_code_groups", T_U32, TT["groups"])]
    for pre, layers in (("talker", TT["layers"]), ("code_pred", TT["cp_layers"])):
        tmeta += [("qwen3-tts.%s.embedding_length" % pre, T_U32, D),
                  ("qwen3-tts.%s.feed_forward_length" % pre, T_U32, TT["ffn"]),
                  ("qwen3-tts.%s.block_count" % pre, T_U32, layers),
                  ("qwen3-tts.%s.attention.head_count" % pre, T_U32, TT["heads"]),
                  ("qwen3-tts.%s.attention.head_count_kv" % pre, T_U32, TT["kv"]),
                  ("qwen3-tts.%s.attention.key_length" % pre, T_U32, TT["hd"]),
                  ("qwen3-tts.%s.rope.freq_base" % pre, T_F32, TT["theta"]),
                  ("qwen3-tts.%s.attention.layer_norm_rms_epsilon" % pre, T_F32, TT["eps"])]
    tmeta += [("qwen3-tts.codec.%s_id" % k, T_U32, v) for k, v in TID.items()]
    tmeta += [("qwen3-tts.codec.language_names", T_ARR, (T_STR, list(TLANG))),
              ("qwen3-tts.codec.language_ids", T_ARR, (T_I32, list(TLANG.values()))),
              ("qwen3-tts.codec.speaker_names", T_ARR, (T_STR, list(TSPK))),
              ("qwen3-tts.codec.speaker_ids", T_ARR, (T_I32, list(TSPK.values())))]
    tmeta += [("qwen3-tts.text.tts_pad_id", T_U32, nt - 3), ("qwen3-tts.text.tts_bos_id", T_U32, nt - 2),
              ("qwen3-tts.text.tts_eos_id", T_U32, nt - 1)] + tok_meta
    talker = (tmeta, b.tensors)
    W = b.W
    c, C = TTSBuilder(rng), TC
    c.add("tok_dec.vq_first.0.codebook", "F32", [C["cb_dim"], TT["codes"]], 1.0)
    for k in range(TT["groups"] - 1):
        c.add("tok_dec.vq_rest.%d.codebook" % k, "F32", [C["cb_dim"], TT["codes"]], 0.5)
    c.add("tok_dec.vq_first.output_proj.weight", "F16", [1, C["cb_dim"], C["hidden"]], 0.25)
    c.add("tok_dec.vq_rest.output_proj.weight", "F32", [1, C["cb_dim"], C["hidden"]], 0.25)
    c.add("tok_dec.pre_conv.weight", "F16", [3, C["hidden"], C["latent"]], 0.15)
    c.add("tok_dec.pre_conv.bias", "F32", [C["latent"]], 0.1)
    P = "tok_dec.pre_tfm."
    c.add(P + "input_proj.weight", "F16", [C["latent"], C["hidden"]], 0.12)
    c.add(P + "input_proj.bias", "F32", [C["hidden"]], 0.1)
    qd = C["heads"] * C["hd"]
    for l in range(C["layers"]):
        p = P + "blk.%d." % l
        c.add(p + "attn_norm.weight", "F32", [C["hidden"]], 0.1, 1.0)
        c.add(p + "ffn_norm.weight", "F32", [C["hidden"]], 0.1, 1.0)
        c.add(p + "attn_q.weight", "F16", [C["hidden"], qd], 0.2)
        c.add(p + "attn_k.weight", "F32", [C["hidden"], qd], 0.2)
        c.add(p + "attn_v.weight", "F16", [C["hidden"], qd], 0.2)
        c.add(p + "attn_output.weight", "F16", [qd, C["hidden"]], 0.2)
        c.add(p + "ffn_gate.weight", "F16", [C["hidden"], C["ffn"]], 0.2)
        c.add(p + "ffn_up.weight", "Q8_0" if l == 0 else "F16", [C["hidden"], C["ffn"]], 0.2)
        c.add(p + "ffn_down.weight", "F16", [C["ffn"], C["hidden"]], 0.15)
        c.add(p + "attn_scale", "F32", [C["hidden"]], 0.1, 0.5)
        c.add(p + "ffn_scale", "F32", [C["hidden"]], 0.1, 0.5)
    c.add(P + "norm.weight", "F32", [C["hidden"]], 0.1, 1.0)
    c.add(P + "output_proj.weight", "F16", [C["hidden"], C["latent"]], 0.2)
    c.add(P + "output_proj.bias", "F32", [C["latent"]], 0.1)
    L = C["latent"]
    for i in range(2):
        U = "tok_dec.upsample.%d." % i
        c.add(U + "conv.weight", "F16", [2, L, L], 0.12)
        c.add(U + "conv.bias", "F32", [L], 0.1)
        c.add(U + "dwconv.weight", "F16", [7, 1, L], 0.3)
        c.add(U + "dwconv.bias", "F32", [L], 0.1)
        c.add(U + "norm.weight", "F32", [L], 0.1, 1.0)
        c.add(U + "norm.bias", "F32", [L], 0.1)
        c.add(U + "pwconv1.weight", "F16", [L, 4 * L], 0.12)
        c.add(U + "pwconv1.bias", "F32", [4 * L], 0.1)
        c.add(U + "pwconv2.weight", "F32", [4 * L, L], 0.06)
        c.add(U + "pwconv2.bias", "F32", [L], 0.1)
        c.add(U + "gamma", "F32", [L], 0.2, 0.5)
    ch = C["dec"]
    c.add("tok_dec.dec.0.conv.weight", "F16", [7, L, ch], 0.05)
    c.add("tok_dec.dec.0.conv.bias", "F32", [ch], 0.1)
    for i, s in enumerate((8, 5, 4, 3)):
        B, out = "tok_dec.dec.%d." % (i + 1), ch // 2
        c.add(B + "snake.alpha", "F32", [ch], 0.3)
        c.add(B + "snake.beta", "F32", [ch], 0.3)
        c.add(B + "conv_t.weight", "F16" if i % 2 == 0 else "F32", [2 * s, out, ch], 0.4 / math.sqrt(ch))
        c.add(B + "conv_t.bias", "F32", [out], 0.05)
        for r in range(3):
            R = B + "res.%d." % r
            for a in ("act1", "act2"):
                c.add(R + a + ".alpha", "F32", [out], 0.3)
                c.add(R + a + ".beta", "F32", [out], 0.3)
            c.add(R + "conv1.weight", "F16", [7, out, out], 0.3 / math.sqrt(7 * out))
            c.add(R + "conv1.bias", "F32", [out], 0.05)
            c.add(R + "conv2.weight", "BF16", [1, out, out], 0.3 / math.sqrt(out))
            c.add(R + "conv2.bias", "F32", [out], 0.05)
        ch = out
    c.add("tok_dec.dec.5.snake.alpha", "F32", [ch], 0.3)
    c.add("tok_dec.dec.5.snake.beta", "F32", [ch], 0.3)
    c.add("tok_dec.dec.6.conv.weight", "F16", [7, ch, 1], 0.15)
    c.add("tok_dec.dec.6.conv.bias", "F32", [1], 0.02)
    D_ = "qwen3-tts-tokenizer.decoder."
    cmeta = [("general.architecture", T_STR, "qwen3-tts-tokenizer"), ("general.alignment", T_U32, 32),
             (D_ + "num_quantizers", T_U32, TT["groups"]), (D_ + "codebook_size", T_U32, TT["codes"]),
             (D_ + "codebook_dim_internal", T_U32, C["cb_dim"]), (D_ + "hidden_size", T_U32, C["hidden"]),
             (D_ + "vector_quantization_hidden_dim", T_U32, C["hidden"]), (D_ + "latent_dim", T_U32, L),
             (D_ + "decoder_dim", T_U32, C["dec"]), (D_ + "sliding_window", T_U32, C["window"]),
             (D_ + "num_attention_heads", T_U32, C["heads"]), (D_ + "head_dim", T_U32, C["hd"]),
             (D_ + "num_hidden_layers", T_U32, C["layers"]), (D_ + "intermediate_size", T_U32, C["ffn"]),
             (D_ + "rms_norm_eps", T_F32, C["eps"]), (D_ + "rope_theta", T_F32, C["theta"])]
    W.update(c.W)
    return talker, (cmeta, c.tensors), W, vocab, ranks


def make_tts(out):
    text, frames = "hello world", 6
    seed = 1
    while True:
        rng = np.random.default_rng(2000 + seed)
        talker, codec, W, vocab, ranks = tts_build(rng)
        role = tokenize("<|im_start|>assistant\n", vocab, ranks)
        codes, gap = tts_codes(W, role, tokenize(text, vocab, ranks), "english", "bob", frames)
        if gap > 0.02 and len(codes) == frames:
            pcm = tts_decode(W, codes)
            peak = float(np.abs(pcm).max())
            if 0.05 < peak < 0.95 and abs(peak * 100 - math.floor(peak * 100) - 0.5) < 0.3:
                break
        seed += 1
    write_gguf(out + "/tts.gguf", *talker)
    write_gguf(out + "/ttscodec.gguf", *codec)
    cmd = "ai say --voice /mnt/hdb/tts.gguf --codec /mnt/hdb/ttscodec.gguf --greedy --frames %d --speaker bob " \
          "--lang english %s" % (frames, text)
    return [cmd + " | grep first", "first codes: " + " ".join(map(str, codes[0])),
            cmd + " | grep leading", "leading codes: " + " ".join(str(c[0]) for c in codes),
            cmd + " -o /home/said.wav | grep peak", "peak %.2f" % peak]


def main():
    out = sys.argv[1]
    cases = make(out, "qwen2", "tiny", True) + make(out, "qwen3", "tiny3", False) + make(out, "llama", "tinyl", False) + \
        make(out, "starcoder2", "tinysc", False) + make(out, "minicpm", "tinymc", False) + \
        make(out, "phi3", "tinyphi", False) + make(out, "gemma", "tinyg", False) + \
        make(out, "gemma2", "tinyg2", False) + make(out, "gemma3", "tinyg3", False)
    # a whole turn of the assistant (system prompt with the tools, the chat format) on the last model loaded
    cases += ["ai ask -n 5 --no-tools hello there", "[5 tokens, 0 tool calls"]
    cases += make_whisper(out)
    cases += make_tts(out)
    open(out + "/cases.txt", "w").write("\n".join(cases) + "\n")


if __name__ == "__main__":
    main()
