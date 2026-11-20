/* Speech to text: Whisper (OpenAI's speech recognition model) in the file format of whisper.cpp (ggml-*.bin): the
   sizes, the mel filters, the words, then the tensors one after another. Sound at 16 kHz becomes a log-mel picture
   (80 bands, 100 frames a second; a window of 2 * audio context frames, 30 s for the published models), two
   convolutions and transformer blocks encode it, and a decoder writes the words one token at a time, looking at the
   encoding (cross-attention). Greedy: the likeliest token each time. The matrices are multiplied as in llm.h. */

#define WH_RATE 16000
#define WH_HOP 160
#define WH_FFT 400
#define WH_MAX_TOKENS 224
#define WH_MAX_TENSORS 512

struct wh_attn {
    struct ai_tensor q, k, v, o;
    float *bq, *bv, *bo;
};

struct wh_block {
    float *ln1_w, *ln1_b, *lnx_w, *lnx_b, *ln2_w, *ln2_b, *b1, *b2;
    struct wh_attn self, cross;
    struct ai_tensor fc1, fc2;
};

struct wh_raw {
    const char *name;
    int name_len, dims[4], nd;
    uint32_t type;
    const uint8_t *data;
};

struct whisper {
    uint8_t *file;
    size_t size;
    char path[160];
    int n_vocab, a_ctx, a_dim, a_heads, a_layers, t_ctx, t_dim, t_heads, t_layers, n_mels, n_fft;
    int eot, sot, translate, transcribe, no_timestamps, langs;
    bool multilingual;
    float *filters, *window, *dft_cos, *dft_sin;
    const uint8_t **words;
    int *word_len, n_words;
    struct wh_raw *raw;
    int n_raw;
    float *conv1_w, *conv1_b, *conv2_w, *conv2_b, *a_pos, *t_pos, *enc_ln_w, *enc_ln_b, *dec_ln_w, *dec_ln_b;
    struct wh_block *enc, *dec;
    struct ai_tensor tok_emb;
    /* the decoder's state */
    int a_used; /* audio positions the last encoding filled: fewer than a_ctx for short speech */
    float *kc, *vc, *xk, *xv; /* self-attention cache [layer][t_ctx][dim], cross keys/values [layer][a_ctx][dim] */
    int n_past;
    float *logits;
};

static const char *const wh_lang_codes[] = {
    "en", "zh", "de", "es", "ru", "ko", "fr", "ja", "pt", "tr", "pl", "ca", "nl", "ar", "sv", "it", "id", "hi", "fi",
    "vi", "he", "uk", "el", "ms", "cs", "ro", "da", "hu", "ta", "no", "th", "ur", "hr", "bg", "lt", "la", "mi", "ml",
    "cy", "sk", "te", "fa", "lv", "bn", "sr", "az", "sl", "kn", "et", "mk", "br", "eu", "is", "hy", "ne", "mn", "bs",
    "kk", "sq", "sw", "gl", "mr", "pa", "si", "km", "sn", "yo", "so", "af", "oc", "ka", "be", "tg", "sd", "gu", "am",
    "yi", "lo", "uz", "fo", "ht", "ps", "tk", "nn", "mt", "sa", "lb", "my", "bo", "tl", "mg", "as", "tt", "haw", "ln",
    "ha", "ba", "jw", "su", "yue"};

static void wh_free(struct whisper *w)
{
    if (w->enc)
        for (int i = 0; i < w->a_layers; i++) {
            struct wh_block *b = &w->enc[i];
            float *p[] = {b->ln1_w, b->ln1_b, b->ln2_w, b->ln2_b, b->b1, b->b2, b->self.bq, b->self.bv, b->self.bo};
            for (size_t k = 0; k < ARRAY_SIZE(p); k++)
                kfree(p[k]);
        }
    if (w->dec)
        for (int i = 0; i < w->t_layers; i++) {
            struct wh_block *b = &w->dec[i];
            float *p[] = {b->ln1_w,   b->ln1_b,   b->lnx_w,    b->lnx_b,    b->ln2_w,    b->ln2_b,   b->b1,
                          b->b2,      b->self.bq, b->self.bv,  b->self.bo,  b->cross.bq, b->cross.bv, b->cross.bo};
            for (size_t k = 0; k < ARRAY_SIZE(p); k++)
                kfree(p[k]);
        }
    float *p[] = {w->filters, w->window,   w->dft_cos,  w->dft_sin,  w->conv1_w,  w->conv1_b,  w->conv2_w,
                  w->conv2_b, w->a_pos,    w->t_pos,    w->enc_ln_w, w->enc_ln_b, w->dec_ln_w, w->dec_ln_b,
                  w->kc,      w->vc,       w->xk,       w->xv,       w->logits};
    for (size_t k = 0; k < ARRAY_SIZE(p); k++)
        kfree(p[k]);
    kfree(w->enc);
    kfree(w->dec);
    kfree(w->words);
    kfree(w->word_len);
    kfree(w->raw);
    kfree(w->file);
    memset(w, 0, sizeof(*w));
}

static const struct wh_raw *wh_find(const struct whisper *w, const char *name)
{
    size_t n = strlen(name);
    for (int i = 0; i < w->n_raw; i++)
        if ((size_t)w->raw[i].name_len == n && !memcmp(w->raw[i].name, name, n))
            return &w->raw[i];
    return NULL;
}

static int wh_count(const struct wh_raw *r)
{
    int n = 1;
    for (int i = 0; i < r->nd; i++)
        n *= r->dims[i];
    return n;
}

/* a tensor of any type as floats (small ones: norms, biases, convolutions, positions) */
static float *wh_floats(const struct whisper *w, const char *name, int want, bool *ok)
{
    const struct wh_raw *r = wh_find(w, name);
    if (!r || wh_count(r) != want) {
        *ok = false;
        return NULL;
    }
    float *out = kmalloc(sizeof(float) * (size_t)want);
    if (!out) {
        *ok = false;
        return NULL;
    }
    deq_row(r->type, r->data, out, want);
    return out;
}

static bool wh_matrix(const struct whisper *w, const char *name, int cols, int rows, struct ai_tensor *t)
{
    const struct wh_raw *r = wh_find(w, name);
    if (!r || r->nd != 2 || r->dims[0] != cols || r->dims[1] != rows)
        return false;
    int bytes = 0, per = gguf_block(r->type, &bytes);
    if (!per || cols % per)
        return false;
    *t = (struct ai_tensor){r->data, r->type, cols, rows, (size_t)(cols / per) * (size_t)bytes, NULL};
    return true;
}

static bool wh_attn_load(const struct whisper *w, const char *pre, int dim, struct wh_attn *a)
{
    char n[96];
    bool ok = true;
    ksnprintf(n, sizeof(n), "%s.query.weight", pre);
    ok &= wh_matrix(w, n, dim, dim, &a->q);
    ksnprintf(n, sizeof(n), "%s.key.weight", pre);
    ok &= wh_matrix(w, n, dim, dim, &a->k);
    ksnprintf(n, sizeof(n), "%s.value.weight", pre);
    ok &= wh_matrix(w, n, dim, dim, &a->v);
    ksnprintf(n, sizeof(n), "%s.out.weight", pre);
    ok &= wh_matrix(w, n, dim, dim, &a->o);
    ksnprintf(n, sizeof(n), "%s.query.bias", pre);
    a->bq = wh_floats(w, n, dim, &ok);
    ksnprintf(n, sizeof(n), "%s.value.bias", pre);
    a->bv = wh_floats(w, n, dim, &ok);
    ksnprintf(n, sizeof(n), "%s.out.bias", pre);
    a->bo = wh_floats(w, n, dim, &ok);
    return ok;
}

static bool wh_block_load(const struct whisper *w, const char *pre, int dim, bool cross, struct wh_block *b)
{
    char n[96];
    bool ok = true;
#define WF(field, suffix, count)                                                                                       \
    ksnprintf(n, sizeof(n), "%s." suffix, pre);                                                                        \
    b->field = wh_floats(w, n, count, &ok)
    WF(ln1_w, "attn_ln.weight", dim);
    WF(ln1_b, "attn_ln.bias", dim);
    WF(ln2_w, "mlp_ln.weight", dim);
    WF(ln2_b, "mlp_ln.bias", dim);
    WF(b1, "mlp.0.bias", 4 * dim);
    WF(b2, "mlp.2.bias", dim);
    if (cross) {
        WF(lnx_w, "cross_attn_ln.weight", dim);
        WF(lnx_b, "cross_attn_ln.bias", dim);
    }
#undef WF
    ksnprintf(n, sizeof(n), "%s.attn", pre);
    ok &= wh_attn_load(w, n, dim, &b->self);
    if (cross) {
        ksnprintf(n, sizeof(n), "%s.cross_attn", pre);
        ok &= wh_attn_load(w, n, dim, &b->cross);
    }
    ksnprintf(n, sizeof(n), "%s.mlp.0.weight", pre);
    ok &= wh_matrix(w, n, dim, 4 * dim, &b->fc1);
    ksnprintf(n, sizeof(n), "%s.mlp.2.weight", pre);
    ok &= wh_matrix(w, n, 4 * dim, dim, &b->fc2);
    return ok;
}

/* takes file (kmalloc'd) on success */
static int wh_load(struct whisper *w, uint8_t *file, size_t size, char *why, size_t cap)
{
    qdot_choose();
    memset(w, 0, sizeof(*w));
    half_init();
    struct gguf_reader r = {file, file + size, false};
#define POS ((size_t)(r.p - file))
    int32_t h[12];
    for (int i = 0; i < 12; i++)
        h[i] = (int32_t)rd_u32(&r);
    if (r.bad || (uint32_t)h[0] != 0x67676d6cu) {
        str_copy(why, "not a whisper.cpp model (ggml-*.bin)", cap);
        return E_INVAL;
    }
    w->n_vocab = h[1], w->a_ctx = h[2], w->a_dim = h[3], w->a_heads = h[4], w->a_layers = h[5];
    w->t_ctx = h[6], w->t_dim = h[7], w->t_heads = h[8], w->t_layers = h[9], w->n_mels = h[10];
    if (w->a_ctx <= 0 || w->a_ctx > 1500 || w->a_dim <= 0 || w->a_dim > 4096 || w->a_heads <= 0 ||
        w->a_dim % w->a_heads || w->a_layers <= 0 || w->a_layers > 64 || w->t_ctx <= 4 || w->t_ctx > 448 ||
        w->t_dim != w->a_dim || w->t_heads <= 0 || w->t_dim % w->t_heads || w->t_layers <= 0 || w->t_layers > 64 ||
        w->n_mels <= 0 || w->n_mels > 128 || w->n_vocab < 1700 || w->n_vocab > 60000) {
        str_copy(why, "this whisper model has sizes it cannot have", cap);
        return E_INVAL;
    }
    int nm = (int)rd_u32(&r);
    w->n_fft = (int)rd_u32(&r);
    if (r.bad || nm != w->n_mels || w->n_fft != WH_FFT / 2 + 1 || POS + (size_t)nm * (size_t)w->n_fft * 4 > size) {
        str_copy(why, "the mel filters of the model are not for 400-point windows", cap);
        return E_INVAL;
    }
    w->filters = kmalloc(sizeof(float) * (size_t)nm * (size_t)w->n_fft);
    if (!w->filters)
        goto nomem;
    memcpy(w->filters, file + POS, sizeof(float) * (size_t)nm * (size_t)w->n_fft);
    r.p += sizeof(float) * (size_t)nm * (size_t)w->n_fft;
    w->n_words = (int)rd_u32(&r);
    if (r.bad || w->n_words <= 0 || w->n_words > w->n_vocab) {
        str_copy(why, "the words of the model are damaged", cap);
        goto bad;
    }
    w->words = kmalloc(sizeof(*w->words) * (size_t)w->n_words);
    w->word_len = kmalloc(sizeof(int) * (size_t)w->n_words);
    if (!w->words || !w->word_len)
        goto nomem;
    for (int i = 0; i < w->n_words; i++) {
        uint32_t len = rd_u32(&r);
        if (r.bad || len > 256 || POS + len > size) {
            str_copy(why, "the words of the model are damaged", cap);
            goto bad;
        }
        w->words[i] = file + POS;
        w->word_len[i] = (int)len;
        r.p += len;
    }
    /* the special tokens follow the words: end, start, the languages, translate, transcribe, ..., no timestamps */
    w->multilingual = w->n_vocab != 51864;
    w->langs = w->n_vocab == 51866 ? 100 : 99;
    w->eot = w->n_vocab - 1509 - w->langs;
    w->sot = w->eot + 1;
    w->translate = w->sot + 1 + w->langs;
    w->transcribe = w->translate + 1;
    w->no_timestamps = w->transcribe + 4;
    w->raw = kmalloc(sizeof(*w->raw) * WH_MAX_TENSORS);
    if (!w->raw)
        goto nomem;
    while (POS < size && w->n_raw < WH_MAX_TENSORS) {
        struct wh_raw *t = &w->raw[w->n_raw];
        t->nd = (int)rd_u32(&r);
        t->name_len = (int)rd_u32(&r);
        t->type = rd_u32(&r);
        if (r.bad || t->nd < 1 || t->nd > 4 || t->name_len <= 0 || t->name_len > 80)
            break;
        uint64_t count = 1;
        for (int i = 0; i < t->nd; i++) {
            t->dims[i] = (int)rd_u32(&r);
            if (t->dims[i] <= 0)
                r.bad = true;
            count *= (uint64_t)(t->dims[i] > 0 ? t->dims[i] : 1);
        }
        if (r.bad || POS + (size_t)t->name_len > size)
            break;
        t->name = (const char *)file + POS;
        r.p += (size_t)t->name_len;
        int bytes = 0, per = gguf_block(t->type, &bytes);
        if (!per || count % (uint64_t)per) {
            ksnprintf(why, cap, "tensor %.*s: type %u is not supported", t->name_len, t->name, (unsigned)t->type);
            goto bad;
        }
        uint64_t len = count / (uint64_t)per * (uint64_t)bytes;
        if (POS + len > size)
            break;
        t->data = file + POS;
        r.p += (size_t)len;
        w->n_raw++;
    }
    bool ok = true;
    int D = w->a_dim;
    w->conv1_w = wh_floats(w, "encoder.conv1.weight", 3 * w->n_mels * D, &ok);
    w->conv1_b = wh_floats(w, "encoder.conv1.bias", D, &ok);
    w->conv2_w = wh_floats(w, "encoder.conv2.weight", 3 * D * D, &ok);
    w->conv2_b = wh_floats(w, "encoder.conv2.bias", D, &ok);
    w->a_pos = wh_floats(w, "encoder.positional_embedding", w->a_ctx * D, &ok);
    w->t_pos = wh_floats(w, "decoder.positional_embedding", w->t_ctx * D, &ok);
    w->enc_ln_w = wh_floats(w, "encoder.ln_post.weight", D, &ok);
    w->enc_ln_b = wh_floats(w, "encoder.ln_post.bias", D, &ok);
    w->dec_ln_w = wh_floats(w, "decoder.ln.weight", D, &ok);
    w->dec_ln_b = wh_floats(w, "decoder.ln.bias", D, &ok);
    ok &= wh_matrix(w, "decoder.token_embedding.weight", D, w->n_vocab, &w->tok_emb);
    w->enc = kzalloc(sizeof(*w->enc) * (size_t)w->a_layers);
    w->dec = kzalloc(sizeof(*w->dec) * (size_t)w->t_layers);
    if (!w->enc || !w->dec)
        goto nomem;
    char pre[48];
    for (int i = 0; i < w->a_layers && ok; i++) {
        ksnprintf(pre, sizeof(pre), "encoder.blocks.%d", i);
        ok = wh_block_load(w, pre, D, false, &w->enc[i]);
    }
    for (int i = 0; i < w->t_layers && ok; i++) {
        ksnprintf(pre, sizeof(pre), "decoder.blocks.%d", i);
        ok = wh_block_load(w, pre, D, true, &w->dec[i]);
    }
    if (!ok) {
        str_copy(why, "the model lacks a tensor whisper needs, or one has the wrong size", cap);
        goto bad;
    }
    /* the window and the 400-point transform as tables */
    w->window = kmalloc(sizeof(float) * WH_FFT);
    w->dft_cos = kmalloc(sizeof(float) * WH_FFT);
    w->dft_sin = kmalloc(sizeof(float) * WH_FFT);
    w->kc = kzalloc(sizeof(float) * (size_t)w->t_layers * (size_t)w->t_ctx * (size_t)D);
    w->vc = kzalloc(sizeof(float) * (size_t)w->t_layers * (size_t)w->t_ctx * (size_t)D);
    w->xk = kzalloc(sizeof(float) * (size_t)w->t_layers * (size_t)w->a_ctx * (size_t)D);
    w->xv = kzalloc(sizeof(float) * (size_t)w->t_layers * (size_t)w->a_ctx * (size_t)D);
    w->logits = kmalloc(sizeof(float) * (size_t)w->n_vocab);
    if (!w->window || !w->dft_cos || !w->dft_sin || !w->kc || !w->vc || !w->xk || !w->xv || !w->logits)
        goto nomem;
    for (int i = 0; i < WH_FFT; i++) {
        double s, c;
        ai_sincos(2 * 3.14159265358979323846 * i / WH_FFT, &s, &c);
        w->window[i] = (float)(0.5 - 0.5 * c);
        w->dft_cos[i] = (float)c;
        w->dft_sin[i] = (float)s;
    }
    w->file = file;
    w->size = size;
    return 0;
#undef POS
nomem:
    str_copy(why, "not enough memory for the speech model", cap);
    w->file = NULL;
    wh_free(w);
    return E_NOMEM;
bad:
    w->file = NULL;
    wh_free(w);
    return E_INVAL;
}

/* ---- the log-mel picture ---- */

struct mel_job {
    const struct whisper *w;
    const float *pcm;
    int n, frames;
    float *mel; /* [n_mels][frames] */
};

static void mel_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit");
#endif
    const struct mel_job *j = ctx;
    const struct whisper *w = j->w;
    float frame[WH_FFT], power[WH_FFT / 2 + 1];
    for (int t = from; t < to; t++) {
        /* centred: frame t covers samples t * hop - 200 ... + 199, mirrored at the start, zeros after the end */
        bool any = false;
        for (int i = 0; i < WH_FFT; i++) {
            int at = t * WH_HOP - WH_FFT / 2 + i;
            if (at < 0)
                at = -at;
            float v = at < j->n ? j->pcm[at] : 0.0f;
            any |= v != 0.0f;
            frame[i] = v * w->window[i];
        }
        for (int f = 0; f < w->n_fft; f++) {
            float re = 0, im = 0;
            if (any)
                for (int i = 0, k = 0; i < WH_FFT; i++, k += f) {
                    if (k >= WH_FFT)
                        k -= WH_FFT;
                    re += frame[i] * w->dft_cos[k];
                    im -= frame[i] * w->dft_sin[k];
                }
            power[f] = re * re + im * im;
        }
        for (int m = 0; m < w->n_mels; m++) {
            float s = dot(w->filters + (size_t)m * (size_t)w->n_fft, power, w->n_fft);
            j->mel[(size_t)m * (size_t)j->frames + (size_t)t] = s;
        }
    }
}

static void log_mel(const struct whisper *w, const float *pcm, int n, float *mel, int frames)
{
    struct mel_job j = {w, pcm, n, frames, mel};
    hal_parallel(mel_part, &j, frames, 8);
    float top = -1e30f;
    int count = w->n_mels * frames;
    for (int i = 0; i < count; i++) {
        float v = mel[i] < 1e-10f ? 1e-10f : mel[i];
        mel[i] = (float)(ai_log(v) / 2.302585092994046);
        if (mel[i] > top)
            top = mel[i];
    }
    for (int i = 0; i < count; i++)
        mel[i] = ((mel[i] < top - 8.0f ? top - 8.0f : mel[i]) + 4.0f) / 4.0f;
}

/* ---- the transformer pieces ---- */

static void layernorm(float *out, const float *x, const float *g, const float *b, int n)
{
    float mean = 0, var = 0;
    for (int i = 0; i < n; i++)
        mean += x[i];
    mean /= (float)n;
    for (int i = 0; i < n; i++)
        var += (x[i] - mean) * (x[i] - mean);
    float s = 1.0f / ai_sqrtf(var / (float)n + 1e-5f);
    for (int i = 0; i < n; i++)
        out[i] = (x[i] - mean) * s * g[i] + b[i];
}

static inline float gelu(float x)
{
    float u = 0.7978845608f * (x + 0.044715f * x * x * x);
    float t = u > 10.0f ? 1.0f : u < -10.0f ? -1.0f : 1.0f - 2.0f / (ai_expf(2.0f * u) + 1.0f);
    return 0.5f * x * (1.0f + t);
}

/* out[t] = W in[t] + b for n rows of in, AI_BATCH at a time */
static void wh_linear(const struct ai_tensor *w, const float *bias, const float *in, float *out, int n)
{
    for (int t = 0; t < n; t += AI_BATCH) {
        int b = MIN(AI_BATCH, n - t);
        matmul(w, in + (size_t)t * (size_t)w->cols, w->cols, out + (size_t)t * (size_t)w->rows, w->rows, b);
    }
    if (bias)
        for (int t = 0; t < n; t++)
            for (int r = 0; r < w->rows; r++)
                out[(size_t)t * (size_t)w->rows + (size_t)r] += bias[r];
}

struct conv_job {
    const float *w, *b, *in;
    float *out;
    int cin, len_in, len_out, stride;
};

/* out[c][t] = gelu(b[c] + sum over i and k of w[c][i][k] in[i][t * stride + k - 1]) */
static void conv_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit");
#endif
    const struct conv_job *j = ctx;
    for (int c = from; c < to; c++) {
        float *o = j->out + (size_t)c * (size_t)j->len_out;
        for (int t = 0; t < j->len_out; t++)
            o[t] = j->b[c];
        for (int i = 0; i < j->cin; i++) {
            const float *k = j->w + ((size_t)c * (size_t)j->cin + (size_t)i) * 3, *x = j->in + (size_t)i * (size_t)j->len_in;
            for (int t = 0; t < j->len_out; t++) {
                int at = t * j->stride - 1;
                float s = 0;
                if (at >= 0)
                    s += k[0] * x[at];
                s += k[1] * x[at + 1];
                if (at + 2 < j->len_in)
                    s += k[2] * x[at + 2];
                o[t] += s;
            }
        }
        for (int t = 0; t < j->len_out; t++)
            o[t] = gelu(o[t]);
    }
}

struct wh_att_job {
    const float *q, *k, *v;
    float *out;
    int nq, nk, dim, heads, q0; /* q0: the position of the first query, for the causal mask (-1: none) */
};

/* one pass over the keys with the softmax kept as it goes (the running maximum rescales what came before), so the
   helper processors need no memory of their own: the heap is not theirs to use */
static void wh_att_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit");
#endif
    const struct wh_att_job *j = ctx;
    int hd = j->dim / j->heads;
    float scale = 1.0f / ai_sqrtf((float)hd);
    for (int item = from; item < to; item++) {
        int h = item % j->heads, t = item / j->heads;
        const float *q = j->q + (size_t)t * (size_t)j->dim + (size_t)(h * hd);
        int nk = j->q0 >= 0 ? MIN(j->nk, j->q0 + t + 1) : j->nk;
        float *o = j->out + (size_t)t * (size_t)j->dim + (size_t)(h * hd);
        float best = -1e30f, sum = 0;
        for (int d = 0; d < hd; d++)
            o[d] = 0;
        for (int s = 0; s < nk; s++) {
            float sc = dot(q, j->k + (size_t)s * (size_t)j->dim + (size_t)(h * hd), hd) * scale;
            const float *v = j->v + (size_t)s * (size_t)j->dim + (size_t)(h * hd);
            if (sc > best) {
                float shrink = ai_expf(best - sc);
                sum *= shrink;
                for (int d = 0; d < hd; d++)
                    o[d] *= shrink;
                best = sc;
            }
            float p = ai_expf(sc - best);
            sum += p;
            for (int d = 0; d < hd; d++)
                o[d] += p * v[d];
        }
        for (int d = 0; d < hd; d++)
            o[d] /= sum;
    }
}

static void wh_attend(const float *q, const float *k, const float *v, float *out, int nq, int nk, int dim, int heads,
                      int q0)
{
    struct wh_att_job j = {q, k, v, out, nq, nk, dim, heads, q0};
    hal_parallel(wh_att_part, &j, nq * heads, 4);
}

/* the encoder: mel [n_mels][2 * T] to the keys and values every decoder block looks at; T is at most a_ctx, and
   short speech needs only the start of the 30 s the model was made for, which is many times quicker */
static int wh_encode(struct whisper *w, const float *mel, int T)
{
    int D = w->a_dim, F = 2 * T;
    float *h1 = kmalloc(sizeof(float) * (size_t)D * (size_t)F), *x = kmalloc(sizeof(float) * (size_t)D * (size_t)T),
          *xb = kmalloc(sizeof(float) * (size_t)D * (size_t)T), *q = kmalloc(sizeof(float) * (size_t)D * (size_t)T),
          *k = kmalloc(sizeof(float) * (size_t)D * (size_t)T), *v = kmalloc(sizeof(float) * (size_t)D * (size_t)T),
          *hid = kmalloc(sizeof(float) * 4 * (size_t)D * (size_t)T);
    int err = 0;
    if (!h1 || !x || !xb || !q || !k || !v || !hid) {
        err = E_NOMEM;
        goto out;
    }
    struct conv_job c1 = {w->conv1_w, w->conv1_b, mel, h1, w->n_mels, F, F, 1};
    hal_parallel(conv_part, &c1, D, 4);
    struct conv_job c2 = {w->conv2_w, w->conv2_b, h1, xb, D, F, T, 2};
    hal_parallel(conv_part, &c2, D, 4);
    for (int t = 0; t < T; t++)
        for (int c = 0; c < D; c++)
            x[(size_t)t * (size_t)D + (size_t)c] = xb[(size_t)c * (size_t)T + (size_t)t] + w->a_pos[(size_t)t * (size_t)D + (size_t)c];
    for (int l = 0; l < w->a_layers; l++) {
        const struct wh_block *b = &w->enc[l];
        for (int t = 0; t < T; t++)
            layernorm(xb + (size_t)t * (size_t)D, x + (size_t)t * (size_t)D, b->ln1_w, b->ln1_b, D);
        wh_linear(&b->self.q, b->self.bq, xb, q, T);
        wh_linear(&b->self.k, NULL, xb, k, T);
        wh_linear(&b->self.v, b->self.bv, xb, v, T);
        wh_attend(q, k, v, xb, T, T, D, w->a_heads, -1);
        wh_linear(&b->self.o, b->self.bo, xb, q, T);
        for (size_t i = 0; i < (size_t)T * (size_t)D; i++)
            x[i] += q[i];
        for (int t = 0; t < T; t++)
            layernorm(xb + (size_t)t * (size_t)D, x + (size_t)t * (size_t)D, b->ln2_w, b->ln2_b, D);
        wh_linear(&b->fc1, b->b1, xb, hid, T);
        for (size_t i = 0; i < (size_t)T * 4 * (size_t)D; i++)
            hid[i] = gelu(hid[i]);
        wh_linear(&b->fc2, b->b2, hid, q, T);
        for (size_t i = 0; i < (size_t)T * (size_t)D; i++)
            x[i] += q[i];
    }
    for (int t = 0; t < T; t++)
        layernorm(x + (size_t)t * (size_t)D, x + (size_t)t * (size_t)D, w->enc_ln_w, w->enc_ln_b, D);
    for (int l = 0; l < w->t_layers; l++) {
        const struct wh_block *b = &w->dec[l];
        wh_linear(&b->cross.k, NULL, x, w->xk + (size_t)l * (size_t)w->a_ctx * (size_t)D, T);
        wh_linear(&b->cross.v, b->cross.bv, x, w->xv + (size_t)l * (size_t)w->a_ctx * (size_t)D, T);
    }
    w->a_used = T;
    w->n_past = 0;
out:
    kfree(h1);
    kfree(x);
    kfree(xb);
    kfree(q);
    kfree(k);
    kfree(v);
    kfree(hid);
    return err;
}

/* one token through the decoder; the logits of the next one in w->logits */
static void wh_decode(struct whisper *w, int token)
{
    int D = w->t_dim, pos = w->n_past;
    float x[4096], xb[4096], q[4096], o[4096], *hid = kmalloc(sizeof(float) * 4 * (size_t)D);
    if (!hid || pos >= w->t_ctx)
        goto out;
    deq_row(w->tok_emb.type, w->tok_emb.data + (size_t)token * w->tok_emb.row_bytes, x, D);
    for (int c = 0; c < D; c++)
        x[c] += w->t_pos[(size_t)pos * (size_t)D + (size_t)c];
    for (int l = 0; l < w->t_layers; l++) {
        const struct wh_block *b = &w->dec[l];
        float *kc = w->kc + (size_t)l * (size_t)w->t_ctx * (size_t)D, *vc = w->vc + (size_t)l * (size_t)w->t_ctx * (size_t)D;
        layernorm(xb, x, b->ln1_w, b->ln1_b, D);
        wh_linear(&b->self.q, b->self.bq, xb, q, 1);
        wh_linear(&b->self.k, NULL, xb, kc + (size_t)pos * (size_t)D, 1);
        wh_linear(&b->self.v, b->self.bv, xb, vc + (size_t)pos * (size_t)D, 1);
        wh_attend(q, kc, vc, o, 1, pos + 1, D, w->t_heads, -1);
        wh_linear(&b->self.o, b->self.bo, o, xb, 1);
        for (int c = 0; c < D; c++)
            x[c] += xb[c];
        layernorm(xb, x, b->lnx_w, b->lnx_b, D);
        wh_linear(&b->cross.q, b->cross.bq, xb, q, 1);
        wh_attend(q, w->xk + (size_t)l * (size_t)w->a_ctx * (size_t)D, w->xv + (size_t)l * (size_t)w->a_ctx * (size_t)D, o,
                  1, w->a_used, D, w->t_heads, -1);
        wh_linear(&b->cross.o, b->cross.bo, o, xb, 1);
        for (int c = 0; c < D; c++)
            x[c] += xb[c];
        layernorm(xb, x, b->ln2_w, b->ln2_b, D);
        wh_linear(&b->fc1, b->b1, xb, hid, 1);
        for (int i = 0; i < 4 * D; i++)
            hid[i] = gelu(hid[i]);
        wh_linear(&b->fc2, b->b2, hid, xb, 1);
        for (int c = 0; c < D; c++)
            x[c] += xb[c];
    }
    layernorm(xb, x, w->dec_ln_w, w->dec_ln_b, D);
    matmul(&w->tok_emb, xb, D, w->logits, w->n_vocab, 1);
    w->n_past++;
out:
    kfree(hid);
}

static int wh_argmax(const float *v, int from, int to)
{
    int best = from;
    for (int i = from + 1; i < to; i++)
        if (v[i] > v[best])
            best = i;
    return best;
}

/* how many audio positions n samples need: what they fill and a margin of silence after it (the model knows the
   end of speech by it), but never less than WH_MIN_CTX, below which the model starts to make words up */
#define WH_MIN_CTX 256
#define WH_CTX_MARGIN 64
static bool wh_full_ctx; /* ai hear --full: all 30 s as the model was made, to compare */
static int wh_ctx_for(const struct whisper *w, int n)
{
    if (wh_full_ctx)
        return w->a_ctx;
    int filled = (n / WH_HOP + 1) / 2 + 1;
    return MIN(w->a_ctx, MAX(WH_MIN_CTX, filled + WH_CTX_MARGIN));
}

/* speech (16 kHz mono floats, at most 2 * a_ctx * 160 samples are heard) to tokens; returns how many; lang gets
   the language heard (-1 for an English-only model) */
static int wh_transcribe(struct whisper *w, const float *pcm, int n, int *tokens, int cap, int *lang)
{
    int T = wh_ctx_for(w, n), frames = 2 * T;
    float *mel = kmalloc(sizeof(float) * (size_t)w->n_mels * (size_t)frames);
    if (!mel)
        return E_NOMEM;
    log_mel(w, pcm, n, mel, frames);
    int err = wh_encode(w, mel, T);
    kfree(mel);
    if (err)
        return err;
    int prompt[4], np = 0;
    *lang = -1;
    prompt[np++] = w->sot;
    if (w->multilingual) {
        wh_decode(w, w->sot);
        *lang = wh_argmax(w->logits, w->sot + 1, w->sot + 1 + w->langs) - (w->sot + 1);
        w->n_past = 0;
        prompt[np++] = w->sot + 1 + *lang;
        prompt[np++] = w->transcribe;
    }
    prompt[np++] = w->no_timestamps;
    for (int i = 0; i < np; i++)
        wh_decode(w, prompt[i]);
    int count = 0;
    while (count < cap && w->n_past < w->t_ctx) {
        int best = wh_argmax(w->logits, 0, w->eot + 1);
        if (!count && best == w->eot) /* something must be said first */
            best = wh_argmax(w->logits, 0, w->eot);
        if (best == w->eot)
            break;
        tokens[count++] = best;
        wh_decode(w, best);
    }
    return count;
}

static void wh_text(const struct whisper *w, const int *tokens, int n, char *out, size_t cap)
{
    size_t len = 0;
    for (int i = 0; i < n; i++)
        if (tokens[i] < w->n_words)
            for (int k = 0; k < w->word_len[tokens[i]] && len + 1 < cap; k++)
                out[len++] = (char)w->words[tokens[i]][k];
    out[len] = 0;
    size_t lead = 0;
    while (out[lead] == ' ')
        lead++;
    memmove(out, out + lead, len - lead + 1);
}
