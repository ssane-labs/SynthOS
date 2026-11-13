/* The byte-level BPE tokenizer of GPT-2 style models (Qwen, Llama 3, SmolLM...): text is split into words the
   way their pre-tokenizer regex does it (letters, digits, punctuation, spaces; the regex's Unicode classes are
   approximated), each word becomes one token per byte and pairs are merged in the order of the merge list.
   Special tokens (<|im_start|>...) are recognised only in text the assistant itself builds. */

struct ai_tok {
    int n;
    const char **text; /* the token strings, as stored (bytes mapped to printable characters) */
    uint16_t *len;
    uint8_t *type;      /* 1 normal, 3 control, 4 user defined, 6 byte */
    uint32_t *hash;     /* text -> id + 1, open addressing */
    uint32_t hash_mask;
    uint64_t *mkey;     /* merges: (left << 32 | right) + 1 -> rank and result */
    uint32_t *mval_rank, *mval_id;
    uint32_t merge_mask;
    int byte_tok[256];
    int digits;         /* how many digits make one word: 1 (Qwen) or 3 (Llama 3) */
    int *specials;      /* control and user-defined tokens, longest first */
    int special_count;
    int eos, eot, bos;  /* end of turn tokens; -1 if none */
};

/* GPT-2's byte -> character map: printable Latin-1 bytes are themselves, the rest move to 256 and up */
static uint16_t b2u[256];
static uint8_t u2b[512];

static void b2u_init(void)
{
    int n = 0;
    for (int b = 0; b < 256; b++) {
        bool keep = (b >= '!' && b <= '~') || (b >= 0xa1 && b <= 0xac) || (b >= 0xae && b <= 0xff);
        b2u[b] = (uint16_t)(keep ? b : 256 + n++);
        u2b[b2u[b]] = (uint8_t)b;
    }
}

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int tok_find(const struct ai_tok *t, const char *s, size_t n)
{
    for (uint32_t i = fnv(s, n) & t->hash_mask;; i = (i + 1) & t->hash_mask) {
        uint32_t v = t->hash[i];
        if (!v)
            return -1;
        if (t->len[v - 1] == n && !memcmp(t->text[v - 1], s, n))
            return (int)v - 1;
    }
}

static uint32_t mix64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* rank and result of merging left and right; false if they do not merge */
static bool tok_merge(const struct ai_tok *t, int left, int right, uint32_t *rank, int *id)
{
    if (!t->mkey)
        return false;
    uint64_t key = ((uint64_t)(uint32_t)left << 32 | (uint32_t)right) + 1;
    for (uint32_t i = mix64(key) & t->merge_mask;; i = (i + 1) & t->merge_mask) {
        if (!t->mkey[i])
            return false;
        if (t->mkey[i] == key) {
            *rank = t->mval_rank[i];
            *id = (int)t->mval_id[i];
            return true;
        }
    }
}

static int utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    out[0] = (char)(0xe0 | cp >> 12);
    out[1] = (char)(0x80 | ((cp >> 6) & 63));
    out[2] = (char)(0x80 | (cp & 63));
    return 3;
}

/* the code point at s (n bytes left) and its length; bad bytes stand for themselves */
static uint32_t utf8_at(const char *s, size_t n, int *len)
{
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80 || n < 2) {
        *len = 1;
        return u[0];
    }
    if ((u[0] & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
        *len = 2;
        return (uint32_t)(u[0] & 31) << 6 | (u[1] & 63);
    }
    if ((u[0] & 0xf0) == 0xe0 && n >= 3 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
        *len = 3;
        return (uint32_t)(u[0] & 15) << 12 | (uint32_t)(u[1] & 63) << 6 | (u[2] & 63);
    }
    if ((u[0] & 0xf8) == 0xf0 && n >= 4 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80 &&
        (u[3] & 0xc0) == 0x80) {
        *len = 4;
        return (uint32_t)(u[0] & 7) << 18 | (uint32_t)(u[1] & 63) << 12 | (uint32_t)(u[2] & 63) << 6 | (u[3] & 63);
    }
    *len = 1;
    return u[0];
}

static void tok_free(struct ai_tok *t)
{
    kfree(t->text);
    kfree(t->len);
    kfree(t->type);
    kfree(t->hash);
    kfree(t->mkey);
    kfree(t->mval_rank);
    kfree(t->mval_id);
    kfree(t->specials);
    memset(t, 0, sizeof(*t));
}

static int tok_load(struct ai_tok *t, const struct gguf *g, char *why, size_t cap)
{
    memset(t, 0, sizeof(*t));
    b2u_init();
    struct gguf_str model = gv_str(gguf_find(g, "tokenizer.ggml.model"));
    if (!gs_eq(model, "gpt2")) {
        ksnprintf(why, cap, "the model's tokenizer (%.*s) is not supported; use a Qwen, Llama 3 or SmolLM model",
                  (int)MIN(model.n, 20), model.s);
        return E_NOSYS;
    }
    struct gguf_str pre = gv_str(gguf_find(g, "tokenizer.ggml.pre"));
    t->digits = gs_eq(pre, "qwen2") || gs_eq(pre, "deepseek-llm") ? 1 : 3;
    uint32_t ty;
    uint64_t n;
    const uint8_t *items;
    if (!gv_array(gguf_find(g, "tokenizer.ggml.tokens"), &ty, &n, &items) || ty != GV_STR || !n || n > 1000000) {
        str_copy(why, "the model has no vocabulary", cap);
        return E_INVAL;
    }
    t->n = (int)n;
    uint32_t hsize = 1;
    while (hsize < n * 2)
        hsize <<= 1;
    t->text = kmalloc(sizeof(char *) * n);
    t->len = kmalloc(sizeof(uint16_t) * n);
    t->type = kmalloc(n);
    t->hash = kzalloc(sizeof(uint32_t) * hsize);
    t->hash_mask = hsize - 1;
    if (!t->text || !t->len || !t->type || !t->hash) {
        tok_free(t);
        return E_NOMEM;
    }
    struct gguf_reader r = {items, g->buf + g->size, false};
    for (int i = 0; i < t->n; i++) {
        struct gguf_str s = rd_str(&r);
        t->text[i] = s.s;
        t->len[i] = (uint16_t)MIN(s.n, 65535u);
        t->type[i] = 1;
        uint32_t h = fnv(s.s, t->len[i]) & t->hash_mask;
        while (t->hash[h])
            h = (h + 1) & t->hash_mask;
        t->hash[h] = (uint32_t)i + 1;
    }
    if (r.bad) {
        tok_free(t);
        str_copy(why, "the vocabulary is damaged", cap);
        return E_INVAL;
    }
    if (gv_array(gguf_find(g, "tokenizer.ggml.token_type"), &ty, &n, &items) && ty == GV_I32 &&
        n == (uint64_t)t->n)
        for (int i = 0; i < t->n; i++)
            t->type[i] = items[i * 4];
    for (int b = 0; b < 256; b++) {
        char enc[4];
        t->byte_tok[b] = tok_find(t, enc, (size_t)utf8_put(b2u[b], enc));
    }
    if (gv_array(gguf_find(g, "tokenizer.ggml.merges"), &ty, &n, &items) && ty == GV_STR && n) {
        uint32_t msize = 1;
        while (msize < n * 2)
            msize <<= 1;
        t->mkey = kzalloc(sizeof(uint64_t) * msize);
        t->mval_rank = kmalloc(sizeof(uint32_t) * msize);
        t->mval_id = kmalloc(sizeof(uint32_t) * msize);
        t->merge_mask = msize - 1;
        if (!t->mkey || !t->mval_rank || !t->mval_id) {
            tok_free(t);
            return E_NOMEM;
        }
        r = (struct gguf_reader){items, g->buf + g->size, false};
        char both[512];
        for (uint64_t i = 0; i < n && !r.bad; i++) {
            struct gguf_str s = rd_str(&r);
            const char *sp = memchr(s.s, ' ', s.n);
            if (!sp || s.n >= sizeof(both))
                continue;
            size_t ln = (size_t)(sp - s.s), rn = s.n - ln - 1;
            int a = tok_find(t, s.s, ln), b = tok_find(t, sp + 1, rn);
            memcpy(both, s.s, ln);
            memcpy(both + ln, sp + 1, rn);
            int ab = tok_find(t, both, ln + rn);
            if (a < 0 || b < 0 || ab < 0)
                continue;
            uint64_t key = ((uint64_t)(uint32_t)a << 32 | (uint32_t)b) + 1;
            uint32_t h = mix64(key) & t->merge_mask;
            while (t->mkey[h] && t->mkey[h] != key)
                h = (h + 1) & t->merge_mask;
            if (t->mkey[h])
                continue; /* the first (best ranked) rule wins */
            t->mkey[h] = key;
            t->mval_rank[h] = (uint32_t)i;
            t->mval_id[h] = (uint32_t)ab;
        }
    }
    int count = 0;
    for (int i = 0; i < t->n; i++)
        count += (t->type[i] == 3 || t->type[i] == 4) && t->len[i] > 1;
    t->specials = kmalloc(sizeof(int) * (size_t)(count + 1));
    if (!t->specials) {
        tok_free(t);
        return E_NOMEM;
    }
    for (int i = 0; i < t->n; i++)
        if ((t->type[i] == 3 || t->type[i] == 4) && t->len[i] > 1) {
            int k = t->special_count++;
            while (k > 0 && t->len[t->specials[k - 1]] < t->len[i]) { /* longest first */
                t->specials[k] = t->specials[k - 1];
                k--;
            }
            t->specials[k] = i;
        }
    t->bos = (int)gv_uint(gguf_find(g, "tokenizer.ggml.bos_token_id"), (uint64_t)-1);
    t->eos = (int)gv_uint(gguf_find(g, "tokenizer.ggml.eos_token_id"), (uint64_t)-1);
    t->eot = tok_find(t, "<|im_end|>", 10);
    if (t->eot < 0)
        t->eot = tok_find(t, "<|eot_id|>", 10);
    if (t->eot < 0)
        t->eot = tok_find(t, "<|end|>", 7);
    return 0;
}

/* ---- the Qwen2 / Llama 3 split ---- */

static bool cp_letter(uint32_t c)
{
    if (c < 128)
        return (c | 32) >= 'a' && (c | 32) <= 'z';
    return !((c >= 0x2000 && c <= 0x206f) || (c >= 0x3000 && c <= 0x303f) || (c >= 0xff00 && c <= 0xff0f) ||
             c == 0x85 || (c >= 0x80 && c <= 0xbf));
}

static bool cp_number(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static bool cp_space(uint32_t c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0xa0 || c == 0x2028 || c == 0x2029 || c == 0x3000;
}

static bool cp_newline(uint32_t c)
{
    return c == '\r' || c == '\n';
}

/* the code points of text, decoded once */
struct cps {
    uint32_t *cp;
    int *at; /* byte offset of each, plus the end */
    int n;
};

/* the length (in code points) of the word that starts at i */
static int split_word(const struct cps *c, int i, int digits)
{
    const uint32_t *t = c->cp;
    int n = c->n;
    uint32_t ch = t[i];
    if (ch == '\'' && i + 1 < n) {
        uint32_t a = t[i + 1] | 32, b = i + 2 < n ? t[i + 2] | 32 : 0;
        if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l'))
            return 3;
        if (a == 's' || a == 't' || a == 'm' || a == 'd')
            return 2;
    }
    int j = i;
    if (!(cp_newline(ch) || cp_letter(ch) || cp_number(ch)) && i + 1 < n && cp_letter(t[i + 1]))
        j = i + 1;
    if (cp_letter(t[j])) {
        int k = j;
        while (k < n && cp_letter(t[k]))
            k++;
        return k - i;
    }
    if (cp_number(ch)) {
        int k = i;
        while (k < n && k - i < digits && cp_number(t[k]))
            k++;
        return k - i;
    }
    j = ch == ' ' && i + 1 < n && !(cp_space(t[i + 1]) || cp_letter(t[i + 1]) || cp_number(t[i + 1])) ? i + 1 : i;
    if (!(cp_space(t[j]) || cp_letter(t[j]) || cp_number(t[j]))) {
        int k = j;
        while (k < n && !(cp_space(t[k]) || cp_letter(t[k]) || cp_number(t[k])))
            k++;
        while (k < n && cp_newline(t[k]))
            k++;
        return k - i;
    }
    int k = i, last_nl = -1;
    while (k < n && cp_space(t[k])) {
        if (cp_newline(t[k]))
            last_nl = k;
        k++;
    }
    if (last_nl >= 0)
        return last_nl + 1 - i;
    if (k < n && k - i > 1)
        return k - 1 - i;
    return k - i;
}

/* appends the BPE tokens of one word (bytes) to out; returns the new count */
static int bpe_word(const struct ai_tok *t, const char *s, int len, int *out, int count, int cap)
{
    int buf[256];
    int *sym = len <= 256 ? buf : kmalloc(sizeof(int) * (size_t)len);
    if (!sym)
        return count;
    int n = 0;
    for (int i = 0; i < len; i++) {
        int id = t->byte_tok[(uint8_t)s[i]];
        if (id >= 0)
            sym[n++] = id;
    }
    while (n > 1) {
        uint32_t best = 0xffffffffu, rank;
        int at = -1, best_id = -1, id;
        for (int i = 0; i + 1 < n; i++)
            if (tok_merge(t, sym[i], sym[i + 1], &rank, &id) && rank < best) {
                best = rank;
                at = i;
                best_id = id;
            }
        if (at < 0)
            break;
        sym[at] = best_id;
        memmove(sym + at + 1, sym + at + 2, sizeof(int) * (size_t)(n - at - 2));
        n--;
    }
    for (int i = 0; i < n && count < cap; i++)
        out[count++] = sym[i];
    if (sym != buf)
        kfree(sym);
    return count;
}

/* text to tokens; with specials, <|...|> tokens in the text are recognised. Returns the count (at most cap). */
static int tok_encode(const struct ai_tok *t, const char *text, bool specials, int *out, int cap)
{
    size_t total = strlen(text);
    int count = 0;
    size_t pos = 0;
    while (pos < total && count < cap) {
        /* the plain stretch up to the next special token */
        size_t end = total;
        int special = -1;
        if (specials)
            for (size_t p = pos; p < total && special < 0; p++)
                for (int k = 0; k < t->special_count; k++) {
                    int id = t->specials[k];
                    if (t->text[id][0] == text[p] && t->len[id] <= total - p &&
                        !memcmp(text + p, t->text[id], t->len[id])) {
                        end = p;
                        special = id;
                        break;
                    }
                }
        if (end > pos) {
            struct cps c = {kmalloc(sizeof(uint32_t) * (end - pos + 1)), kmalloc(sizeof(int) * (end - pos + 2)), 0};
            if (!c.cp || !c.at) {
                kfree(c.cp);
                kfree(c.at);
                return count;
            }
            for (size_t p = pos; p < end;) {
                int l;
                c.at[c.n] = (int)p;
                c.cp[c.n++] = utf8_at(text + p, end - p, &l);
                p += (size_t)l;
            }
            c.at[c.n] = (int)end;
            for (int i = 0; i < c.n && count < cap;) {
                int w = split_word(&c, i, t->digits);
                if (w < 1)
                    w = 1;
                count = bpe_word(t, text + c.at[i], c.at[i + w] - c.at[i], out, count, cap);
                i += w;
            }
            kfree(c.cp);
            kfree(c.at);
        }
        if (special >= 0 && count < cap) {
            out[count++] = special;
            end += t->len[special];
        }
        pos = end;
    }
    return count;
}

/* the bytes of one token (control tokens give nothing); returns the length */
static int tok_piece(const struct ai_tok *t, int id, char *out, int cap)
{
    if (id < 0 || id >= t->n || t->type[id] == 3)
        return 0;
    const char *s = t->text[id];
    int n = 0;
    for (size_t i = 0; i < t->len[id] && n < cap;) {
        int l;
        uint32_t cp = utf8_at(s + i, t->len[id] - i, &l);
        i += (size_t)l;
        if (t->type[id] == 4) { /* user-defined tokens are plain text */
            n += l <= cap - n ? (memcpy(out + n, s + i - (size_t)l, (size_t)l), l) : 0;
            continue;
        }
        out[n++] = cp < 512 ? (char)u2b[cp] : '?';
    }
    return n;
}
