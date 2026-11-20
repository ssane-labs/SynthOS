/* Speech from text: Qwen3-TTS with its 12 Hz codec (Apache-2.0, two GGUF files: the talker and the codec).
   The talker is a Qwen3 decoder fed with vectors instead of tokens: the text goes in through its own embedding and
   a small projection, and every step it picks the first of 16 codes for 80 ms of sound; a second, five-layer Qwen3
   (the code predictor) picks the other 15 one after another. The codec turns each frame of 16 codes into 1920
   samples at 24 kHz: codebooks, a convolution, a small transformer with a sliding window, two upsampling ConvNeXt
   blocks and a DAC-style decoder (transposed convolutions, Snake activations, dilated residual units).
   Everything in the codec is causal, so it runs as a stream: each layer keeps the few past inputs it still needs and
   the transformer keeps keys and values of the last frames, and the sound of a frame comes out as soon as the frame
   is made, the same samples as decoding the whole utterance at once. The engine is written from the model's
   description; nothing of other implementations is copied. */

#define TTS_RATE 24000
#define TTS_HOP 1920        /* samples per frame of codes */
#define TTS_MAX_GROUPS 16
#define TTS_CHUNK 4         /* frames decoded together */
#define TTS_NAMES 16

/* ---- small numerical helpers ---- */

static float tts_sinf(float x)
{
    const float two_pi = 6.28318530718f, pi = 3.14159265359f, half_pi = 1.57079632679f;
    float k = x / two_pi;
    k = (float)(int)(k + (k >= 0 ? 0.5f : -0.5f));
    float r = x - k * two_pi; /* [-pi, pi] */
    if (r > half_pi)
        r = pi - r;
    else if (r < -half_pi)
        r = -pi - r;
    float r2 = r * r;
    return r * (1.0f + r2 * (-1.6666667e-1f + r2 * (8.3333333e-3f + r2 * (-1.9841270e-4f + r2 * (2.7557319e-6f +
                                                                                                r2 * -2.5052108e-8f)))));
}

/* the error function to about 1e-7 (Abramowitz and Stegun 7.1.26), for the exact GELU */
static float tts_erf(float x)
{
    float a = x < 0 ? -x : x, t = 1.0f / (1.0f + 0.3275911f * a);
    float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t +
                      0.254829592f) * t * ai_expf(-a * a);
    return x < 0 ? -y : y;
}

/* a matrix or vector of the file as floats (vectors, norms, small tables) */
static float *tts_floats(const struct gguf *g, const char *name, size_t n, char *why, size_t cap)
{
    const struct gguf_tensor *t = gguf_tensor(g, name);
    size_t have = t ? (size_t)(t->ne[0] * MAX(t->ne[1], 1ull) * MAX(t->ne[2], 1ull)) : 0;
    if (!t || have != n) {
        ksnprintf(why, cap, t ? "tensor %s has an unexpected shape" : "the model has no tensor %s", name);
        return NULL;
    }
    float *v = kmalloc(sizeof(float) * n);
    if (!v) {
        str_copy(why, "not enough memory", cap);
        return NULL;
    }
    int bytes = 0, per = gguf_block(t->type, &bytes);
    if (!per || n % (size_t)per) {
        ksnprintf(why, cap, "tensor %s has an unsupported type", name);
        kfree(v);
        return NULL;
    }
    deq_row(t->type, t->data, v, (int)n);
    return v;
}

/* a matrix of rows x cols to multiply with (any type the engine reads) */
static bool tts_matrix(const struct gguf *g, const char *name, int cols, int rows, struct ai_tensor *m, char *why,
                       size_t cap)
{
    const struct gguf_tensor *t = gguf_tensor(g, name);
    if (!t || t->ne[0] * MAX(t->ne[1], 1ull) * MAX(t->ne[2], 1ull) != (uint64_t)cols * (uint64_t)rows ||
        t->ne[0] != (uint64_t)(t->dims == 3 ? t->ne[0] : (uint64_t)cols)) {
        ksnprintf(why, cap, t ? "tensor %s has an unexpected shape" : "the model has no tensor %s", name);
        return false;
    }
    int bytes = 0, per = gguf_block(t->type, &bytes);
    if (!per || cols % per) {
        ksnprintf(why, cap, "tensor %s has an unsupported type", name);
        return false;
    }
    *m = (struct ai_tensor){t->data, t->type, cols, rows, (size_t)(cols / per * bytes), NULL};
    return true;
}

/* ---- the codec's layers, each keeping what it needs of the past ---- */

/* a causal convolution; with stride > 1 a transposed one (upsampling by stride). The weights are rearranged in
   the file's memory once, so the inputs of one output are side by side: [cout][k][cin], or for the transposed
   kind [phase][cout][tap][cin]. */
struct tconv {
    struct ai_tensor w;
    float *b;
    int cin, cout, k, dil, stride, taps, keep;
    float *hist; /* the last keep input rows */
};

static int tconv_load(const struct gguf *g, const char *name, int cin, int cout, int k, int dil, int stride,
                      struct tconv *c, char *why, size_t cap)
{
    char n[96];
    ksnprintf(n, sizeof(n), "%s.weight", name);
    const struct gguf_tensor *t = gguf_tensor(g, n);
    bool tr = stride > 1;
    if (!t || t->ne[0] != (uint64_t)k || t->ne[1] != (uint64_t)(tr ? cout : cin) ||
        t->ne[2] != (uint64_t)(tr ? cin : cout) || (tr && k % stride)) {
        ksnprintf(why, cap, t ? "tensor %s has an unexpected shape" : "the model has no tensor %s", n);
        return E_INVAL;
    }
    int es = 0, per = gguf_block(t->type, &es);
    if (per != 1) {
        ksnprintf(why, cap, "tensor %s must be F32, F16 or BF16", n);
        return E_INVAL;
    }
    memset(c, 0, sizeof(*c));
    c->cin = cin;
    c->cout = cout;
    c->k = k;
    c->dil = dil;
    c->stride = stride;
    c->taps = tr ? k / stride : k;
    c->keep = tr ? c->taps - 1 : (k - 1) * dil;
    size_t total = (size_t)k * (size_t)cin * (size_t)cout * (size_t)es;
    uint8_t *data = (uint8_t *)(uintptr_t)t->data, *tmp = big_alloc(total);
    if (!tmp) {
        str_copy(why, "not enough memory", cap);
        return E_NOMEM;
    }
    memcpy(tmp, data, total);
    for (int co = 0; co < cout; co++)
        for (int ci = 0; ci < cin; ci++)
            for (int kk = 0; kk < k; kk++) {
                size_t from, to;
                if (tr) {
                    int r = kk % stride, m = kk / stride;
                    from = ((size_t)ci * (size_t)cout + (size_t)co) * (size_t)k + (size_t)kk;
                    to = (((size_t)r * (size_t)cout + (size_t)co) * (size_t)c->taps + (size_t)m) * (size_t)cin +
                         (size_t)ci;
                } else {
                    from = ((size_t)co * (size_t)cin + (size_t)ci) * (size_t)k + (size_t)kk;
                    to = ((size_t)co * (size_t)k + (size_t)kk) * (size_t)cin + (size_t)ci;
                }
                memcpy(data + to * (size_t)es, tmp + from * (size_t)es, (size_t)es);
            }
    big_free(tmp);
    int cols = c->taps * cin;
    c->w = (struct ai_tensor){data, t->type, cols, cout, (size_t)cols * (size_t)es, NULL};
    ksnprintf(n, sizeof(n), "%s.bias", name);
    if (!(c->b = tts_floats(g, n, (size_t)cout, why, cap)))
        return E_INVAL;
    if (c->keep && !(c->hist = kzalloc(sizeof(float) * (size_t)c->keep * (size_t)cin))) {
        str_copy(why, "not enough memory", cap);
        return E_NOMEM;
    }
    return 0;
}

static void tconv_free(struct tconv *c)
{
    kfree(c->b);
    kfree(c->hist);
    memset(c, 0, sizeof(*c));
}

/* working buffers of the codec, grown as needed */
static float *tts_cat, *tts_col;
static size_t tts_cat_cap, tts_col_cap;

static bool tts_room(float **buf, size_t *cap, size_t n)
{
    if (n <= *cap)
        return true;
    big_free(*buf);
    *buf = big_alloc(sizeof(float) * n);
    *cap = *buf ? n : 0;
    return *buf != NULL;
}

/* in: T rows of cin; out: T * stride rows of cout */
static int tconv_run(struct tconv *c, const float *in, int T, float *out)
{
    int P = c->keep, cin = c->cin, cols = c->w.cols;
    if (!tts_room(&tts_cat, &tts_cat_cap, (size_t)(P + T) * (size_t)cin) ||
        !tts_room(&tts_col, &tts_col_cap, (size_t)AI_BATCH * (size_t)cols))
        return E_NOMEM;
    memcpy(tts_cat, c->hist, sizeof(float) * (size_t)P * (size_t)cin);
    memcpy(tts_cat + (size_t)P * (size_t)cin, in, sizeof(float) * (size_t)T * (size_t)cin);
    for (int t0 = 0; t0 < T; t0 += AI_BATCH) {
        int nb = MIN(AI_BATCH, T - t0);
        for (int j = 0; j < nb; j++)
            for (int m = 0; m < c->taps; m++) {
                int back = c->stride > 1 ? m : (c->k - 1 - m) * c->dil;
                memcpy(tts_col + (size_t)j * (size_t)cols + (size_t)m * (size_t)cin,
                       tts_cat + (size_t)(P + t0 + j - back) * (size_t)cin, sizeof(float) * (size_t)cin);
            }
        for (int r = 0; r < c->stride; r++) {
            struct ai_tensor w = c->w;
            w.data += (size_t)r * (size_t)c->cout * w.row_bytes;
            float *o = out + ((size_t)t0 * (size_t)c->stride + (size_t)r) * (size_t)c->cout;
            matmul(&w, tts_col, cols, o, c->stride * c->cout, nb);
            for (int j = 0; j < nb; j++)
                for (int i = 0; i < c->cout; i++)
                    o[(size_t)j * (size_t)c->stride * (size_t)c->cout + (size_t)i] += c->b[i];
        }
    }
    if (P)
        memcpy(c->hist, tts_cat + (size_t)T * (size_t)cin, sizeof(float) * (size_t)P * (size_t)cin);
    return 0;
}

/* Snake: x + sin^2(a x) / b, per channel; a and 1/b are kept ready */
struct snake {
    float *a, *ib;
};

static int snake_load(const struct gguf *g, const char *name, int ch, struct snake *s, char *why, size_t cap)
{
    char n[96];
    ksnprintf(n, sizeof(n), "%s.alpha", name);
    s->a = tts_floats(g, n, (size_t)ch, why, cap);
    ksnprintf(n, sizeof(n), "%s.beta", name);
    s->ib = s->a ? tts_floats(g, n, (size_t)ch, why, cap) : NULL;
    if (!s->ib)
        return E_INVAL;
    for (int i = 0; i < ch; i++) {
        s->a[i] = ai_expf(s->a[i]);
        s->ib[i] = 1.0f / (ai_expf(s->ib[i]) + 1e-9f);
    }
    return 0;
}

struct snake_job {
    const struct snake *s;
    const float *in;
    float *out;
    int ch;
};

static void snake_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit");
#endif
    const struct snake_job *j = ctx;
    for (int t = from; t < to; t++)
        for (int i = 0; i < j->ch; i++) {
            size_t at = (size_t)t * (size_t)j->ch + (size_t)i;
            float x = j->in[at], v = tts_sinf(j->s->a[i] * x);
            j->out[at] = x + v * v * j->s->ib[i];
        }
}

static void snake_run(const struct snake *s, const float *in, float *out, int T, int ch)
{
    struct snake_job j = {s, in, out, ch};
    hal_parallel(snake_part, &j, T, MAX(1, 4096 / ch));
}

/* the transformer between the codebooks and the upsampling, causal over a window of frames */
struct ctf_layer {
    struct ai_tensor q, k, v, o, gate, up, down;
    float *attn_norm, *ffn_norm, *attn_scale, *ffn_scale;
};

struct codec {
    int nq, cb_size, cb_dim, hidden, latent, dec_dim, window;
    int heads, hd, layers, ffn;
    float eps, theta;
    float *codebook[TTS_MAX_GROUPS]; /* [cb_size][cb_dim] */
    struct ai_tensor first_proj, rest_proj;
    struct tconv pre_conv;
    struct ai_tensor in_proj, out_proj;
    float *in_proj_b, *out_proj_b, *norm;
    struct ctf_layer L[16];
    float *kc, *vc; /* [layer][window][heads * hd] */
    int pos;        /* frames decoded */
    struct {
        struct tconv up;
        float *dw, *dw_b, *ln_w, *ln_b, *gamma, *hist; /* depthwise [ch][7], its past 6 rows */
        struct ai_tensor pw1, pw2;
        float *pw1_b, *pw2_b;
    } ups[2];
    struct tconv conv_pre, conv_post;
    struct {
        struct snake act;
        struct tconv up;
        struct {
            struct snake a1, a2;
            struct tconv c1, c2;
        } res[3];
    } blk[4];
    struct snake act_post;
    float *buf[4];
    size_t buf_cap;
};

static const int codec_strides[4] = {8, 5, 4, 3};

static void codec_free(struct codec *c)
{
    for (int i = 0; i < TTS_MAX_GROUPS; i++)
        kfree(c->codebook[i]);
    tconv_free(&c->pre_conv);
    kfree(c->in_proj_b);
    kfree(c->out_proj_b);
    kfree(c->norm);
    for (int l = 0; l < 16; l++) {
        kfree(c->L[l].attn_norm);
        kfree(c->L[l].ffn_norm);
        kfree(c->L[l].attn_scale);
        kfree(c->L[l].ffn_scale);
    }
    big_free(c->kc);
    big_free(c->vc);
    for (int i = 0; i < 2; i++) {
        tconv_free(&c->ups[i].up);
        kfree(c->ups[i].dw);
        kfree(c->ups[i].dw_b);
        kfree(c->ups[i].ln_w);
        kfree(c->ups[i].ln_b);
        kfree(c->ups[i].gamma);
        kfree(c->ups[i].hist);
        kfree(c->ups[i].pw1_b);
        kfree(c->ups[i].pw2_b);
    }
    tconv_free(&c->conv_pre);
    tconv_free(&c->conv_post);
    for (int b = 0; b < 4; b++) {
        kfree(c->blk[b].act.a);
        kfree(c->blk[b].act.ib);
        tconv_free(&c->blk[b].up);
        for (int r = 0; r < 3; r++) {
            kfree(c->blk[b].res[r].a1.a);
            kfree(c->blk[b].res[r].a1.ib);
            kfree(c->blk[b].res[r].a2.a);
            kfree(c->blk[b].res[r].a2.ib);
            tconv_free(&c->blk[b].res[r].c1);
            tconv_free(&c->blk[b].res[r].c2);
        }
    }
    kfree(c->act_post.a);
    kfree(c->act_post.ib);
    for (int i = 0; i < 4; i++)
        big_free(c->buf[i]);
    memset(c, 0, sizeof(*c));
}

/* back to the start of an utterance: no past */
static void codec_reset(struct codec *c)
{
    struct tconv *all[2 + 2 + 4 * 7];
    int n = 0;
    all[n++] = &c->pre_conv;
    all[n++] = &c->conv_pre;
    all[n++] = &c->conv_post;
    for (int i = 0; i < 2; i++) {
        all[n++] = &c->ups[i].up;
        memset(c->ups[i].hist, 0, sizeof(float) * 6 * (size_t)c->latent);
    }
    for (int b = 0; b < 4; b++) {
        all[n++] = &c->blk[b].up;
        for (int r = 0; r < 3; r++) {
            all[n++] = &c->blk[b].res[r].c1;
            all[n++] = &c->blk[b].res[r].c2;
        }
    }
    for (int i = 0; i < n; i++)
        if (all[i]->hist)
            memset(all[i]->hist, 0, sizeof(float) * (size_t)all[i]->keep * (size_t)all[i]->cin);
    c->pos = 0;
}

static int codec_load(struct codec *c, const struct gguf *g, char *why, size_t cap)
{
    memset(c, 0, sizeof(*c));
#define CU(key, def) (int)gv_uint(gguf_find(g, "qwen3-tts-tokenizer.decoder." key), def)
    c->nq = CU("num_quantizers", 16);
    c->cb_size = CU("codebook_size", 2048);
    c->cb_dim = CU("codebook_dim_internal", 256);
    c->hidden = CU("hidden_size", 512);
    c->latent = CU("latent_dim", 1024);
    c->dec_dim = CU("decoder_dim", 1536);
    c->window = CU("sliding_window", 72);
    c->heads = CU("num_attention_heads", 16);
    c->hd = CU("head_dim", 64);
    c->layers = CU("num_hidden_layers", 8);
    c->ffn = CU("intermediate_size", 1024);
    int vq_hidden = CU("vector_quantization_hidden_dim", 512);
#undef CU
    c->eps = gv_float(gguf_find(g, "qwen3-tts-tokenizer.decoder.rms_norm_eps"), 1e-5f);
    c->theta = gv_float(gguf_find(g, "qwen3-tts-tokenizer.decoder.rope_theta"), 10000.0f);
    if (c->nq < 2 || c->nq > TTS_MAX_GROUPS || c->layers < 1 || c->layers > 16 || c->window < 1 || c->hd & 1 ||
        c->dec_dim % 16 || vq_hidden != c->hidden || c->window > 256 || c->hd > 256) {
        str_copy(why, "the codec's dimensions make no sense", cap);
        return E_INVAL;
    }
    char n[96];
    int err = 0, qd = c->heads * c->hd;
    for (int k = 0; k < c->nq; k++) {
        ksnprintf(n, sizeof(n), k ? "tok_dec.vq_rest.%d.codebook" : "tok_dec.vq_first.%d.codebook", k ? k - 1 : 0);
        if (!(c->codebook[k] = tts_floats(g, n, (size_t)c->cb_size * (size_t)c->cb_dim, why, cap)))
            return E_INVAL;
    }
    bool ok = tts_matrix(g, "tok_dec.vq_first.output_proj.weight", c->cb_dim, c->hidden, &c->first_proj, why, cap) &&
              tts_matrix(g, "tok_dec.vq_rest.output_proj.weight", c->cb_dim, c->hidden, &c->rest_proj, why, cap);
    if (!ok || (err = tconv_load(g, "tok_dec.pre_conv", c->hidden, c->latent, 3, 1, 1, &c->pre_conv, why, cap)))
        return err ? err : E_INVAL;
    ok = tts_matrix(g, "tok_dec.pre_tfm.input_proj.weight", c->latent, c->hidden, &c->in_proj, why, cap) &&
         tts_matrix(g, "tok_dec.pre_tfm.output_proj.weight", c->hidden, c->latent, &c->out_proj, why, cap) &&
         (c->in_proj_b = tts_floats(g, "tok_dec.pre_tfm.input_proj.bias", (size_t)c->hidden, why, cap)) &&
         (c->out_proj_b = tts_floats(g, "tok_dec.pre_tfm.output_proj.bias", (size_t)c->latent, why, cap)) &&
         (c->norm = tts_floats(g, "tok_dec.pre_tfm.norm.weight", (size_t)c->hidden, why, cap));
    for (int l = 0; ok && l < c->layers; l++) {
        struct ctf_layer *L = &c->L[l];
#define M(f, part, cols, rows) (ksnprintf(n, sizeof(n), "tok_dec.pre_tfm.blk.%d." part, l), tts_matrix(g, n, cols, rows, &L->f, why, cap))
#define V(f, part, len) (ksnprintf(n, sizeof(n), "tok_dec.pre_tfm.blk.%d." part, l), (L->f = tts_floats(g, n, (size_t)(len), why, cap)) != NULL)
        ok = M(q, "attn_q.weight", c->hidden, qd) && M(k, "attn_k.weight", c->hidden, qd) &&
             M(v, "attn_v.weight", c->hidden, qd) && M(o, "attn_output.weight", qd, c->hidden) &&
             M(gate, "ffn_gate.weight", c->hidden, c->ffn) && M(up, "ffn_up.weight", c->hidden, c->ffn) &&
             M(down, "ffn_down.weight", c->ffn, c->hidden) && V(attn_norm, "attn_norm.weight", c->hidden) &&
             V(ffn_norm, "ffn_norm.weight", c->hidden) && V(attn_scale, "attn_scale", c->hidden) &&
             V(ffn_scale, "ffn_scale", c->hidden);
#undef M
#undef V
    }
    if (!ok)
        return E_INVAL;
    size_t kv = (size_t)c->layers * (size_t)c->window * (size_t)qd;
    c->kc = big_alloc(sizeof(float) * kv);
    c->vc = big_alloc(sizeof(float) * kv);
    if (!c->kc || !c->vc) {
        str_copy(why, "not enough memory", cap);
        return E_NOMEM;
    }
    for (int i = 0; i < 2; i++) {
        char p[64];
        ksnprintf(p, sizeof(p), "tok_dec.upsample.%d.conv", i);
        if ((err = tconv_load(g, p, c->latent, c->latent, 2, 1, 2, &c->ups[i].up, why, cap)))
            return err;
#define V(f, part, len) (ksnprintf(n, sizeof(n), "tok_dec.upsample.%d." part, i), (c->ups[i].f = tts_floats(g, n, (size_t)(len), why, cap)) != NULL)
        ok = V(dw, "dwconv.weight", 7 * c->latent) && V(dw_b, "dwconv.bias", c->latent) &&
             V(ln_w, "norm.weight", c->latent) && V(ln_b, "norm.bias", c->latent) && V(gamma, "gamma", c->latent) &&
             V(pw1_b, "pwconv1.bias", 4 * c->latent) && V(pw2_b, "pwconv2.bias", c->latent);
#undef V
        ksnprintf(n, sizeof(n), "tok_dec.upsample.%d.pwconv1.weight", i);
        ok = ok && tts_matrix(g, n, c->latent, 4 * c->latent, &c->ups[i].pw1, why, cap);
        ksnprintf(n, sizeof(n), "tok_dec.upsample.%d.pwconv2.weight", i);
        ok = ok && tts_matrix(g, n, 4 * c->latent, c->latent, &c->ups[i].pw2, why, cap);
        if (!ok)
            return E_INVAL;
        if (!(c->ups[i].hist = kzalloc(sizeof(float) * 6 * (size_t)c->latent))) {
            str_copy(why, "not enough memory", cap);
            return E_NOMEM;
        }
    }
    if ((err = tconv_load(g, "tok_dec.dec.0.conv", c->latent, c->dec_dim, 7, 1, 1, &c->conv_pre, why, cap)))
        return err;
    int ch = c->dec_dim;
    for (int b = 0; b < 4; b++) {
        char p[64];
        int out = ch / 2, s = codec_strides[b];
        ksnprintf(p, sizeof(p), "tok_dec.dec.%d.snake", b + 1);
        if ((err = snake_load(g, p, ch, &c->blk[b].act, why, cap)))
            return err;
        ksnprintf(p, sizeof(p), "tok_dec.dec.%d.conv_t", b + 1);
        if ((err = tconv_load(g, p, ch, out, 2 * s, 1, s, &c->blk[b].up, why, cap)))
            return err;
        static const int dil[3] = {1, 3, 9};
        for (int r = 0; r < 3; r++) {
            ksnprintf(p, sizeof(p), "tok_dec.dec.%d.res.%d.act1", b + 1, r);
            if ((err = snake_load(g, p, out, &c->blk[b].res[r].a1, why, cap)))
                return err;
            ksnprintf(p, sizeof(p), "tok_dec.dec.%d.res.%d.act2", b + 1, r);
            if ((err = snake_load(g, p, out, &c->blk[b].res[r].a2, why, cap)))
                return err;
            ksnprintf(p, sizeof(p), "tok_dec.dec.%d.res.%d.conv1", b + 1, r);
            if ((err = tconv_load(g, p, out, out, 7, dil[r], 1, &c->blk[b].res[r].c1, why, cap)))
                return err;
            ksnprintf(p, sizeof(p), "tok_dec.dec.%d.res.%d.conv2", b + 1, r);
            if ((err = tconv_load(g, p, out, out, 1, 1, 1, &c->blk[b].res[r].c2, why, cap)))
                return err;
        }
        ch = out;
    }
    if ((err = snake_load(g, "tok_dec.dec.5.snake", ch, &c->act_post, why, cap)) ||
        (err = tconv_load(g, "tok_dec.dec.6.conv", ch, 1, 7, 1, 1, &c->conv_post, why, cap)))
        return err;
    /* the largest stage: TTS_CHUNK frames at the last block, also the widest before it */
    size_t need = 0;
    int t = 4 * TTS_CHUNK;
    ch = c->dec_dim;
    need = MAX(need, (size_t)t * (size_t)MAX(ch, 4 * c->latent));
    need = MAX(need, (size_t)TTS_CHUNK * (size_t)(4 * qd + 2 * c->ffn + 2 * c->cb_dim + c->latent + c->hidden));
    for (int b = 0; b < 4; b++) {
        t *= codec_strides[b];
        ch /= 2;
        need = MAX(need, (size_t)t * (size_t)ch);
    }
    for (int i = 0; i < 4; i++)
        if (!(c->buf[i] = big_alloc(sizeof(float) * need))) {
            str_copy(why, "not enough memory for the codec", cap);
            return E_NOMEM;
        }
    c->buf_cap = need;
    codec_reset(c);
    return 0;
}

/* the transformer over T new frames x [T][hidden] (in place), positions c->pos.. */
static void codec_transformer(struct codec *c, float *x, int T, float *h, float *q, float *k, float *v, float *att,
                              float *f1, float *f2)
{
    int H = c->hidden, qd = c->heads * c->hd, hd = c->hd, W = c->window;
    float scale = 1.0f / ai_sqrtf((float)hd);
    for (int l = 0; l < c->layers; l++) {
        struct ctf_layer *L = &c->L[l];
        for (int t = 0; t < T; t++)
            rmsnorm(h + (size_t)t * (size_t)H, x + (size_t)t * (size_t)H, L->attn_norm, H, c->eps);
        matmul(&L->q, h, H, q, qd, T);
        matmul(&L->k, h, H, k, qd, T);
        matmul(&L->v, h, H, v, qd, T);
        float *kc = c->kc + (size_t)l * (size_t)W * (size_t)qd, *vc = c->vc + (size_t)l * (size_t)W * (size_t)qd;
        for (int t = 0; t < T; t++) {
            int p = c->pos + t;
            float cs[256];
            for (int i = 0; i < hd / 2; i++) {
                double s, co;
                ai_sincos((double)p * ai_exp(-2.0 * i / hd * ai_log(c->theta)), &s, &co);
                cs[2 * i] = (float)co;
                cs[2 * i + 1] = (float)s;
            }
            for (int a = 0; a < c->heads; a++) {
                rope(q + (size_t)t * (size_t)qd + (size_t)a * (size_t)hd, cs, hd, true);
                rope(k + (size_t)t * (size_t)qd + (size_t)a * (size_t)hd, cs, hd, true);
            }
            memcpy(kc + (size_t)(p % W) * (size_t)qd, k + (size_t)t * (size_t)qd, sizeof(float) * (size_t)qd);
            memcpy(vc + (size_t)(p % W) * (size_t)qd, v + (size_t)t * (size_t)qd, sizeof(float) * (size_t)qd);
            /* this frame sees itself and the window - 1 before it (all written by now: earlier t too) */
            int from = MAX(0, p - W + 1);
            for (int a = 0; a < c->heads; a++) {
                const float *qq = q + (size_t)t * (size_t)qd + (size_t)a * (size_t)hd;
                float sc[256], best = -1e30f, sum = 0;
                for (int s = from; s <= p; s++) {
                    float d = dot(qq, kc + (size_t)(s % W) * (size_t)qd + (size_t)a * (size_t)hd, hd) * scale;
                    sc[s - from] = d;
                    best = MAX(best, d);
                }
                for (int s = 0; s <= p - from; s++)
                    sum += sc[s] = ai_expf(sc[s] - best);
                float *o = att + (size_t)t * (size_t)qd + (size_t)a * (size_t)hd;
                for (int i = 0; i < hd; i++)
                    o[i] = 0;
                for (int s = from; s <= p; s++) {
                    float w = sc[s - from] / sum;
                    const float *vv = vc + (size_t)(s % W) * (size_t)qd + (size_t)a * (size_t)hd;
                    for (int i = 0; i < hd; i++)
                        o[i] += w * vv[i];
                }
            }
        }
        matmul(&L->o, att, qd, h, H, T);
        for (int t = 0; t < T; t++)
            for (int i = 0; i < H; i++)
                x[(size_t)t * (size_t)H + (size_t)i] += h[(size_t)t * (size_t)H + (size_t)i] * L->attn_scale[i];
        for (int t = 0; t < T; t++)
            rmsnorm(h + (size_t)t * (size_t)H, x + (size_t)t * (size_t)H, L->ffn_norm, H, c->eps);
        matmul(&L->gate, h, H, f1, c->ffn, T);
        matmul(&L->up, h, H, f2, c->ffn, T);
        for (size_t i = 0; i < (size_t)T * (size_t)c->ffn; i++)
            f1[i] = f1[i] / (1.0f + ai_expf(-f1[i])) * f2[i];
        matmul(&L->down, f1, c->ffn, h, H, T);
        for (int t = 0; t < T; t++)
            for (int i = 0; i < H; i++)
                x[(size_t)t * (size_t)H + (size_t)i] += h[(size_t)t * (size_t)H + (size_t)i] * L->ffn_scale[i];
    }
}

/* ConvNeXt after an upsampling: x [T][ch] in place, tmp has T * 4 ch */
static void convnext(struct codec *c, int i, float *x, int T, float *y, float *wide)
{
    int ch = c->latent;
    for (int t = 0; t < T; t++)
        for (int k = 0; k < ch; k++) {
            float s = c->ups[i].dw_b[k];
            for (int j = 0; j < 7; j++) {
                int src = t - 6 + j;
                float in = src >= 0 ? x[(size_t)src * (size_t)ch + (size_t)k]
                                    : c->ups[i].hist[(size_t)(6 + src) * (size_t)ch + (size_t)k];
                s += c->ups[i].dw[(size_t)k * 7 + (size_t)j] * in;
            }
            y[(size_t)t * (size_t)ch + (size_t)k] = s;
        }
    /* the past for the next call: the last 6 inputs */
    if (T >= 6)
        memcpy(c->ups[i].hist, x + (size_t)(T - 6) * (size_t)ch, sizeof(float) * 6 * (size_t)ch);
    else {
        memmove(c->ups[i].hist, c->ups[i].hist + (size_t)T * (size_t)ch, sizeof(float) * (size_t)(6 - T) * (size_t)ch);
        memcpy(c->ups[i].hist + (size_t)(6 - T) * (size_t)ch, x, sizeof(float) * (size_t)T * (size_t)ch);
    }
    for (int t = 0; t < T; t++) {
        float *r = y + (size_t)t * (size_t)ch, mean = 0, var = 0;
        for (int k = 0; k < ch; k++)
            mean += r[k];
        mean /= (float)ch;
        for (int k = 0; k < ch; k++)
            var += (r[k] - mean) * (r[k] - mean);
        float inv = 1.0f / ai_sqrtf(var / (float)ch + 1e-6f);
        for (int k = 0; k < ch; k++)
            r[k] = (r[k] - mean) * inv * c->ups[i].ln_w[k] + c->ups[i].ln_b[k];
    }
    matmul(&c->ups[i].pw1, y, ch, wide, 4 * ch, T);
    for (size_t j = 0; j < (size_t)T * 4 * (size_t)ch; j++) {
        float v = wide[j] + c->ups[i].pw1_b[j % (4 * (size_t)ch)];
        wide[j] = 0.5f * v * (1.0f + tts_erf(v * 0.70710678f));
    }
    matmul(&c->ups[i].pw2, wide, 4 * ch, y, ch, T);
    for (int t = 0; t < T; t++)
        for (int k = 0; k < ch; k++) {
            size_t at = (size_t)t * (size_t)ch + (size_t)k;
            x[at] += (y[at] + c->ups[i].pw2_b[k]) * c->ups[i].gamma[k];
        }
}

/* frames [T][nq] codes (T <= TTS_CHUNK) to T * TTS_HOP samples */
static int codec_decode(struct codec *c, const int *codes, int T, float *pcm)
{
    if (T < 1 || T > TTS_CHUNK)
        return E_INVAL;
    float *a = c->buf[0], *b = c->buf[1], *d = c->buf[2], *e = c->buf[3];
    int H = c->hidden, cd = c->cb_dim, err;
    /* codebooks: the first and the rest summed apart, each through its projection */
    float *sum = d, *sum2 = d + (size_t)T * (size_t)cd;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < cd; i++) {
            float s1 = 0, s2 = 0;
            for (int k = 0; k < c->nq; k++) {
                int id = CLAMP(codes[t * c->nq + k], 0, c->cb_size - 1);
                float v = c->codebook[k][(size_t)id * (size_t)cd + (size_t)i];
                if (k)
                    s2 += v;
                else
                    s1 += v;
            }
            sum[(size_t)t * (size_t)cd + (size_t)i] = s1;
            sum2[(size_t)t * (size_t)cd + (size_t)i] = s2;
        }
    matmul(&c->first_proj, sum, cd, a, H, T);
    matmul(&c->rest_proj, sum2, cd, b, H, T);
    for (size_t i = 0; i < (size_t)T * (size_t)H; i++)
        a[i] += b[i];
    if ((err = tconv_run(&c->pre_conv, a, T, b))) /* b: [T][latent] */
        return err;
    matmul(&c->in_proj, b, c->latent, a, H, T); /* a: [T][hidden] */
    add_bias(a, c->in_proj_b, H, T, H);
    {
        int qd = c->heads * c->hd;
        float *q = d, *k = q + (size_t)T * (size_t)qd, *v = k + (size_t)T * (size_t)qd,
              *att = v + (size_t)T * (size_t)qd, *f1 = att + (size_t)T * (size_t)qd,
              *f2 = f1 + (size_t)T * (size_t)c->ffn;
        codec_transformer(c, a, T, b, q, k, v, att, f1, f2);
    }
    c->pos += T;
    for (int t = 0; t < T; t++)
        rmsnorm(b + (size_t)t * (size_t)H, a + (size_t)t * (size_t)H, c->norm, H, c->eps);
    matmul(&c->out_proj, b, H, a, c->latent, T);
    add_bias(a, c->out_proj_b, c->latent, T, c->latent);
    int n = T;
    for (int i = 0; i < 2; i++) {
        if ((err = tconv_run(&c->ups[i].up, a, n, b)))
            return err;
        n *= 2;
        convnext(c, i, b, n, d, e);
        float *s = a;
        a = b;
        b = s;
    }
    if ((err = tconv_run(&c->conv_pre, a, n, b)))
        return err;
    float *x = b, *y = a;
    int ch = c->dec_dim;
    for (int k = 0; k < 4; k++) {
        snake_run(&c->blk[k].act, x, y, n, ch);
        if ((err = tconv_run(&c->blk[k].up, y, n, x)))
            return err;
        n *= codec_strides[k];
        ch /= 2;
        for (int r = 0; r < 3; r++) {
            snake_run(&c->blk[k].res[r].a1, x, y, n, ch);
            if ((err = tconv_run(&c->blk[k].res[r].c1, y, n, d)))
                return err;
            snake_run(&c->blk[k].res[r].a2, d, d, n, ch);
            if ((err = tconv_run(&c->blk[k].res[r].c2, d, n, y)))
                return err;
            for (size_t i = 0; i < (size_t)n * (size_t)ch; i++)
                x[i] += y[i];
        }
    }
    snake_run(&c->act_post, x, y, n, ch);
    if ((err = tconv_run(&c->conv_post, y, n, pcm)))
        return err;
    for (int i = 0; i < n; i++)
        pcm[i] = CLAMP(pcm[i], -1.0f, 1.0f);
    return 0;
}

/* ---- the talker and the code predictor ---- */

struct tts {
    char path[160], codec_path[160], name[64];
    uint8_t *codec_file;
    struct gguf cg;
    struct ai_model tm, cp; /* the code predictor borrows the talker's file */
    struct ai_tensor text_embd, fc1, fc2, cp_embd[TTS_MAX_GROUPS], cp_head[TTS_MAX_GROUPS];
    float *fc1_b, *fc2_b;
    int groups, text_dim, codes; /* codes: values of a code (the codebook's size) */
    int pad, bos, eos, think, nothink, think_bos, think_eos, tts_pad, tts_bos, tts_eos;
    int n_lang, n_spk, lang_id[TTS_NAMES], spk_id[TTS_NAMES];
    char lang[TTS_NAMES][24], spk[TTS_NAMES][24];
    float *pad_vec, *bos_vec, *eos_vec, *in, *emb;
    struct codec codec;
    /* what is being said */
    int *hist, n_hist, frames;
    float temp;
    uint64_t rng;
    bool done;
    int frame[TTS_MAX_GROUPS];
};

static void tts_free(struct tts *s)
{
    codec_free(&s->codec);
    gguf_free(&s->cg);
    big_free(s->codec_file);
    memset(&s->cp.g, 0, sizeof(s->cp.g)); /* the talker's */
    s->cp.file = NULL;
    model_free(&s->cp);
    model_free(&s->tm);
    kfree(s->fc1_b);
    kfree(s->fc2_b);
    kfree(s->pad_vec);
    kfree(s->bos_vec);
    kfree(s->eos_vec);
    kfree(s->in);
    kfree(s->emb);
    kfree(s->hist);
    memset(s, 0, sizeof(*s));
}

/* the dimensions of one of the two transformers, from qwen3-tts.<which>.* */
static bool tts_dims(struct ai_model *m, const struct gguf *g, const char *which, char *why, size_t cap)
{
    char a[40];
    ksnprintf(a, sizeof(a), "qwen3-tts.%s", which);
    m->arch = ARCH_QWEN3;
    str_copy(m->arch_name, "qwen3", sizeof(m->arch_name));
    m->dim = (int)gv_uint(gguf_find_arch(g, a, "embedding_length"), 0);
    m->ffn = (int)gv_uint(gguf_find_arch(g, a, "feed_forward_length"), 0);
    m->layers = (int)gv_uint(gguf_find_arch(g, a, "block_count"), 0);
    m->heads = (int)gv_uint(gguf_find_arch(g, a, "attention.head_count"), 0);
    m->kv_heads = (int)gv_uint(gguf_find_arch(g, a, "attention.head_count_kv"), (uint64_t)m->heads);
    m->hd = (int)gv_uint(gguf_find_arch(g, a, "attention.key_length"), 0);
    m->eps = gv_float(gguf_find_arch(g, a, "attention.layer_norm_rms_epsilon"), 1e-6f);
    if (m->dim <= 0 || m->dim > 8192 || m->ffn <= 0 || m->layers <= 0 || m->layers > 128 || m->heads <= 0 ||
        m->kv_heads <= 0 || m->heads % m->kv_heads || m->hd <= 0 || m->hd > 256 || (m->hd & 1)) {
        ksnprintf(why, cap, "the %s's dimensions make no sense", which);
        return false;
    }
    return true;
}

/* text through the talker's text embedding and projection: n rows of dim into out */
static void tts_text(struct tts *s, const int *ids, int n, float *out)
{
    int td = s->text_dim, dim = s->tm.dim;
    float *e = s->tm.hb, *h = s->tm.hb2; /* the talker's FFN space: AI_BATCH * ffn, enough for a row of each */
    for (int i = 0; i < n; i++) {
        int id = CLAMP(ids[i], 0, s->text_embd.rows - 1);
        deq_row(s->text_embd.type, s->text_embd.data + (size_t)id * s->text_embd.row_bytes, e, td);
        matmul(&s->fc1, e, td, h, s->fc1.rows, 1);
        for (int j = 0; j < s->fc1.rows; j++) {
            float v = h[j] + s->fc1_b[j];
            h[j] = v / (1.0f + ai_expf(-v));
        }
        matmul(&s->fc2, h, s->fc1.rows, out + (size_t)i * (size_t)dim, dim, 1);
        for (int j = 0; j < dim; j++)
            out[(size_t)i * (size_t)dim + (size_t)j] += s->fc2_b[j];
    }
}

static void tts_names(const struct gguf *g, const char *names, const char *ids, char out[][24], int *id, int *n)
{
    uint32_t tn = 0, ti = 0;
    uint64_t cn = 0, ci = 0;
    const uint8_t *pn, *pi;
    *n = 0;
    if (!gv_array(gguf_find(g, names), &tn, &cn, &pn) || !gv_array(gguf_find(g, ids), &ti, &ci, &pi) ||
        tn != GV_STR || (ti != GV_I32 && ti != GV_U32) || cn != ci)
        return;
    for (uint64_t i = 0; i < cn && *n < TTS_NAMES; i++) {
        uint64_t len = 0;
        for (int b = 7; b >= 0; b--)
            len = len << 8 | pn[b];
        ksnprintf(out[*n], 24, "%.*s", (int)MIN(len, 23ull), (const char *)pn + 8);
        pn += 8 + len;
        id[*n] = (int)((uint32_t)pi[0] | (uint32_t)pi[1] << 8 | (uint32_t)pi[2] << 16 | (uint32_t)pi[3] << 24);
        pi += 4;
        (*n)++;
    }
}

/* takes over both files; ctx: frames plus text the talker can hold */
static int tts_load(struct tts *s, uint8_t *file, size_t size, uint8_t *cfile, size_t csize, int ctx, char *why,
                    size_t cap)
{
    qdot_choose();
    memset(s, 0, sizeof(*s));
    s->codec_file = cfile;
    struct ai_model *m = &s->tm;
    int err;
    if (!half_init()) {
        str_copy(why, "not enough memory", cap);
        big_free(file);
        tts_free(s);
        return E_NOMEM;
    }
    m->file = file;
    m->file_size = size;
    if ((err = gguf_parse(&m->g, file, size, why, cap)) || (err = gguf_parse(&s->cg, cfile, csize, why, cap)))
        goto fail;
    err = E_INVAL;
    if (!gs_eq(gv_str(gguf_find(&m->g, "general.architecture")), "qwen3-tts")) {
        str_copy(why, "the voice is not a Qwen3-TTS talker", cap);
        goto fail;
    }
    if (!gs_eq(gv_str(gguf_find(&s->cg, "general.architecture")), "qwen3-tts-tokenizer")) {
        str_copy(why, "the codec is not a Qwen3-TTS tokenizer", cap);
        goto fail;
    }
    struct gguf_str nm = gv_str(gguf_find(&m->g, "general.name"));
    ksnprintf(s->name, sizeof(s->name), "%.*s", (int)MIN(nm.n, 63), nm.s);
    const struct gguf *g = &m->g;
    if (!tts_dims(m, g, "talker", why, cap))
        goto fail;
    if ((err = tok_load(&m->tok, g, why, cap)))
        goto fail;
    err = E_INVAL;
    s->groups = (int)gv_uint(gguf_find(g, "qwen3-tts.num_code_groups"), 16);
    s->codes = s->codec.cb_size = 0;
#define ID(f, key) (s->f = (int)gv_uint(gguf_find(g, key), 0))
    ID(pad, "qwen3-tts.codec.pad_id");
    ID(bos, "qwen3-tts.codec.bos_id");
    ID(eos, "qwen3-tts.codec.eos_id");
    ID(think, "qwen3-tts.codec.think_id");
    ID(nothink, "qwen3-tts.codec.nothink_id");
    ID(think_bos, "qwen3-tts.codec.think_bos_id");
    ID(think_eos, "qwen3-tts.codec.think_eos_id");
    ID(tts_pad, "qwen3-tts.text.tts_pad_id");
    ID(tts_bos, "qwen3-tts.text.tts_bos_id");
    ID(tts_eos, "qwen3-tts.text.tts_eos_id");
#undef ID
    tts_names(g, "qwen3-tts.codec.language_names", "qwen3-tts.codec.language_ids", s->lang, s->lang_id, &s->n_lang);
    tts_names(g, "qwen3-tts.codec.speaker_names", "qwen3-tts.codec.speaker_ids", s->spk, s->spk_id, &s->n_spk);
    const struct gguf_tensor *head = gguf_tensor(g, "talker.codec_head.weight"),
                             *te = gguf_tensor(g, "talker.text_embd.weight"),
                             *f1 = gguf_tensor(g, "talker.text_proj.fc1.weight");
    if (!head || !te || !f1 || s->groups < 2 || s->groups > TTS_MAX_GROUPS) {
        str_copy(why, "the talker misses its codec head, text embedding or projection", cap);
        goto fail;
    }
    m->vocab = (int)head->ne[1];
    s->text_dim = (int)te->ne[0];
    m->ctx = ctx;
    if (!tts_matrix(g, "talker.codec_embd.weight", m->dim, m->vocab, &m->embd, why, cap) ||
        !tts_matrix(g, "talker.codec_head.weight", m->dim, m->vocab, &m->out, why, cap) ||
        !tts_matrix(g, "talker.text_embd.weight", s->text_dim, (int)te->ne[1], &s->text_embd, why, cap) ||
        !tts_matrix(g, "talker.text_proj.fc1.weight", s->text_dim, (int)f1->ne[1], &s->fc1, why, cap) ||
        !tts_matrix(g, "talker.text_proj.fc2.weight", (int)f1->ne[1], m->dim, &s->fc2, why, cap) ||
        !(s->fc1_b = tts_floats(g, "talker.text_proj.fc1.bias", (size_t)f1->ne[1], why, cap)) ||
        !(s->fc2_b = tts_floats(g, "talker.text_proj.fc2.bias", (size_t)m->dim, why, cap)))
        goto fail;
    if ((err = model_blocks(m, "talker.", why, cap)) ||
        (err = model_work(m, gv_float(gguf_find(g, "qwen3-tts.talker.rope.freq_base"), 1e6f), why, cap)))
        goto fail;
    err = E_INVAL;
    if (s->fc1.rows > m->ffn * AI_BATCH || s->text_dim > m->ffn * AI_BATCH) {
        str_copy(why, "the talker's text projection is too wide", cap);
        goto fail;
    }
    /* the code predictor: the same file, its own layers and a head per code */
    struct ai_model *p = &s->cp;
    p->g = m->g;
    p->file = m->file;
    if (!tts_dims(p, g, "code_pred", why, cap) || p->dim != m->dim) {
        if (p->dim != m->dim)
            str_copy(why, "the code predictor needs a projection (a larger talker); not supported yet", cap);
        goto fail;
    }
    const struct gguf_tensor *ch = gguf_tensor(g, "code_pred.lm_head.0.weight");
    if (!ch) {
        str_copy(why, "the model has no tensor code_pred.lm_head.0.weight", cap);
        goto fail;
    }
    s->codes = p->vocab = (int)ch->ne[1];
    p->ctx = s->groups + 2;
    for (int i = 0; i < s->groups - 1; i++) {
        char n[64];
        ksnprintf(n, sizeof(n), "code_pred.codec_embd.%d.weight", i);
        if (!tts_matrix(g, n, p->dim, s->codes, &s->cp_embd[i], why, cap))
            goto fail;
        ksnprintf(n, sizeof(n), "code_pred.lm_head.%d.weight", i);
        if (!tts_matrix(g, n, p->dim, s->codes, &s->cp_head[i], why, cap))
            goto fail;
    }
    p->embd = s->cp_embd[0];
    p->out = s->cp_head[0];
    if ((err = model_blocks(p, "code_pred.", why, cap)) ||
        (err = model_work(p, gv_float(gguf_find(g, "qwen3-tts.code_pred.rope.freq_base"), 1e6f), why, cap)))
        goto fail;
    err = E_NOMEM;
    s->pad_vec = kmalloc(sizeof(float) * (size_t)m->dim);
    s->bos_vec = kmalloc(sizeof(float) * (size_t)m->dim);
    s->eos_vec = kmalloc(sizeof(float) * (size_t)m->dim);
    s->in = kmalloc(sizeof(float) * (size_t)m->dim);
    s->emb = kmalloc(sizeof(float) * (size_t)m->dim);
    s->hist = kmalloc(sizeof(int) * (size_t)m->ctx);
    if (!s->pad_vec || !s->bos_vec || !s->eos_vec || !s->in || !s->emb || !s->hist) {
        str_copy(why, "not enough memory", cap);
        goto fail;
    }
    tts_text(s, &s->tts_pad, 1, s->pad_vec);
    tts_text(s, &s->tts_bos, 1, s->bos_vec);
    tts_text(s, &s->tts_eos, 1, s->eos_vec);
    if ((err = codec_load(&s->codec, &s->cg, why, cap)))
        goto fail;
    if (s->codec.nq != s->groups || s->codec.cb_size != s->codes) {
        str_copy(why, "the codec does not belong to this voice", cap);
        err = E_INVAL;
        goto fail;
    }
    return 0;
fail:
    tts_free(s);
    return err;
}

/* adds row to the input rows of the talker; a batch goes through when full or at the end */
static void tts_row(struct tts *s, const float *row, int *n, bool flush)
{
    struct ai_model *m = &s->tm;
    if (row)
        memcpy(m->x + (size_t)*n * (size_t)m->dim, row, sizeof(float) * (size_t)m->dim);
    *n += row ? 1 : 0;
    if (*n == AI_BATCH || (flush && *n)) {
        forward(m, NULL, *n, flush);
        *n = 0;
    }
}

static int tts_pick(float *lg, int n, struct tts *s, const int *hist, int n_hist, float penalty)
{
    for (int i = 0; i < n_hist; i++) { /* each earlier code once, as the original sampler does */
        int id = hist[i];
        bool seen = false;
        for (int j = 0; j < i && !seen; j++)
            seen = hist[j] == id;
        if (!seen && id >= 0 && id < n)
            lg[id] = lg[id] < 0 ? lg[id] * penalty : lg[id] / penalty;
    }
    int best = 0;
    for (int i = 1; i < n; i++)
        if (lg[i] > lg[best])
            best = i;
    if (s->temp <= 0)
        return best;
    enum { K = 50 };
    int top[K], count = 0;
    for (int i = 0; i < n; i++) {
        if (lg[i] < -1e29f || (count == K && lg[i] <= lg[top[K - 1]]))
            continue;
        int k = count < K ? count++ : K - 1;
        while (k > 0 && lg[top[k - 1]] < lg[i]) {
            top[k] = top[k - 1];
            k--;
        }
        top[k] = i;
    }
    float p[K], sum = 0;
    for (int i = 0; i < count; i++)
        sum += p[i] = ai_expf((lg[top[i]] - lg[top[0]]) / s->temp);
    float r = (float)(rng_next(&s->rng) % 1000000u) / 1000000.0f * sum;
    for (int i = 0; i < count; i++) {
        r -= p[i];
        if (r <= 0)
            return top[i];
    }
    return top[count - 1];
}

/* starts an utterance: the prompt goes through the talker. lang and speaker may be NULL ("auto", the default
   voice); temp 0 picks the likeliest codes (the same sound every time) */
static int tts_begin(struct tts *s, const char *text, const char *lang, const char *speaker, float temp, uint64_t seed,
                     char *why, size_t cap)
{
    struct ai_model *m = &s->tm;
    int ids[512];
    int n = tok_encode(&m->tok, text, false, ids, 400);
    if (n <= 0) {
        str_copy(why, "nothing to say", cap);
        return E_INVAL;
    }
    int left[8], nl = 0, lid = -1, sid = -1;
    for (int i = 0; lang && i < s->n_lang; i++)
        if (!str_casecmp(s->lang[i], lang))
            lid = s->lang_id[i];
    for (int i = 0; speaker && i < s->n_spk; i++)
        if (!str_casecmp(s->spk[i], speaker))
            sid = s->spk_id[i];
    if (lang && strcmp(lang, "auto") && lid < 0) {
        ksnprintf(why, cap, "no language %s in this voice", lang);
        return E_INVAL;
    }
    if (speaker && sid < 0) {
        ksnprintf(why, cap, "no speaker %s in this voice", speaker);
        return E_INVAL;
    }
    if (lid >= 0) {
        left[nl++] = s->think;
        left[nl++] = s->think_bos;
        left[nl++] = lid;
        left[nl++] = s->think_eos;
    } else {
        left[nl++] = s->nothink;
        left[nl++] = s->think_bos;
        left[nl++] = s->think_eos;
    }
    if (sid >= 0)
        left[nl++] = sid;
    left[nl++] = s->pad;
    if (16 + nl + n + 2 + 16 > m->ctx) {
        str_copy(why, "too much text at once", cap);
        return E_RANGE;
    }
    /* "<|im_start|>assistant\n" through the text path, then the codec's own start beside tts_pad / tts_bos,
       then the text with codec_pad, the end of the text, and tts_pad with codec_bos */
    int role[16], nr = tok_encode(&m->tok, "<|im_start|>assistant\n", true, role, 16);
    m->n_past = 0;
    int nb = 0;
    float *row = s->in, *emb = s->emb;
    for (int i = 0; i < nr; i++) {
        tts_text(s, &role[i], 1, row);
        tts_row(s, row, &nb, false);
    }
    for (int i = 0; i < nl; i++) {
        deq_row(m->embd.type, m->embd.data + (size_t)left[i] * m->embd.row_bytes, emb, m->dim);
        const float *t = i == nl - 1 ? s->bos_vec : s->pad_vec;
        for (int j = 0; j < m->dim; j++)
            row[j] = emb[j] + t[j];
        tts_row(s, row, &nb, false);
    }
    deq_row(m->embd.type, m->embd.data + (size_t)s->pad * m->embd.row_bytes, emb, m->dim);
    for (int i = 0; i <= n; i++) {
        if (i < n)
            tts_text(s, &ids[i], 1, row);
        else
            memcpy(row, s->eos_vec, sizeof(float) * (size_t)m->dim);
        for (int j = 0; j < m->dim; j++)
            row[j] += emb[j];
        tts_row(s, row, &nb, false);
    }
    deq_row(m->embd.type, m->embd.data + (size_t)s->bos * m->embd.row_bytes, emb, m->dim);
    for (int j = 0; j < m->dim; j++)
        row[j] = emb[j] + s->pad_vec[j];
    tts_row(s, row, &nb, true);
    s->n_hist = 0;
    s->frames = 0;
    s->done = false;
    s->temp = temp;
    s->rng = seed | 1;
    codec_reset(&s->codec);
    return 0;
}

/* the next frame of 16 codes into s->frame; false at the end of the utterance (or when the talker is full) */
static bool tts_step(struct tts *s)
{
    struct ai_model *m = &s->tm, *p = &s->cp;
    if (s->done || m->n_past + 1 >= m->ctx) {
        s->done = true;
        return false;
    }
    int dim = m->dim;
    float *h = s->in;
    memcpy(h, m->xb, sizeof(float) * (size_t)dim); /* the talker's last state, normalised */
    for (int i = s->codes; i < m->vocab; i++) /* only codes, or the end */
        if (i != s->eos)
            m->logits[i] = -1e30f;
    int c0 = tts_pick(m->logits, m->vocab, s, s->hist, s->n_hist, 1.05f);

    if (c0 == s->eos || c0 >= s->codes) {
        s->done = true;
        return false;
    }
    s->hist[s->n_hist++] = c0;
    s->frame[0] = c0;
    /* the code predictor: the state and the first code, then each code it picks */
    p->n_past = 0;
    memcpy(p->x, h, sizeof(float) * (size_t)dim);
    deq_row(m->embd.type, m->embd.data + (size_t)c0 * m->embd.row_bytes, p->x + dim, dim);
    for (int g = 0; g < s->groups - 1; g++) {
        p->out = s->cp_head[g];
        if (g == 0)
            forward(p, NULL, 2, true);
        else {
            deq_row(s->cp_embd[g - 1].type, s->cp_embd[g - 1].data + (size_t)s->frame[g] * s->cp_embd[g - 1].row_bytes,
                    p->x, dim);
            forward(p, NULL, 1, true);
        }
        s->frame[g + 1] = tts_pick(p->logits, s->codes, s, NULL, 0, 1.0f);
    }
    /* the talker's next input: all 16 codes' embeddings and tts_pad */
    float *x = m->x, *e = m->hb;
    deq_row(m->embd.type, m->embd.data + (size_t)c0 * m->embd.row_bytes, x, dim);
    for (int g = 1; g < s->groups; g++) {
        deq_row(s->cp_embd[g - 1].type, s->cp_embd[g - 1].data + (size_t)s->frame[g] * s->cp_embd[g - 1].row_bytes, e,
                dim);
        for (int j = 0; j < dim; j++)
            x[j] += e[j];
    }
    for (int j = 0; j < dim; j++)
        x[j] += s->pad_vec[j];
    forward(m, NULL, 1, true);
    s->frames++;
    return true;
}
