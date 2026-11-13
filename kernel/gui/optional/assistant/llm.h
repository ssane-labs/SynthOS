/* The language model: a decoder-only transformer of the Llama family as llama.cpp stores it (architectures
   llama, qwen2 and qwen3): RMSNorm, grouped-query attention with rotary positions, a SwiGLU feed-forward block.
   Weights stay quantised in the loaded file; each matrix row is turned into floats block by block while it is
   multiplied, rows are shared out among the processors (hal_parallel). Prompts go through in batches of up to
   AI_BATCH tokens so each row is decoded once per batch. Keys and values of earlier tokens stay in a cache. */

#define AI_BATCH 16

enum { ARCH_LLAMA, ARCH_QWEN2, ARCH_QWEN3 };

struct ai_tensor {
    const uint8_t *data;
    uint32_t type;
    int cols, rows;
    size_t row_bytes;
};

struct ai_layer {
    struct ai_tensor q, k, v, o, gate, up, down;
    float *attn_norm, *ffn_norm, *bq, *bk, *bv, *q_norm, *k_norm;
};

struct ai_model {
    struct gguf g;
    uint8_t *file;
    size_t file_size;
    char path[160], name[64], arch_name[16];
    int arch;
    int dim, ffn, heads, kv_heads, hd, layers, vocab, ctx;
    float eps;
    struct ai_tensor embd, out;
    float *out_norm;
    struct ai_layer *L;
    float *inv_freq;
    float *kc, *vc; /* [layer][ctx][kv_heads * hd] */
    int n_past;
    int *past;      /* the tokens in the cache, to reuse a shared beginning */
    float *x, *xb, *q, *k, *v, *att, *hb, *hb2, *scores, *logits, *rope_cs;
    uint64_t params;
    struct ai_tok tok;
};

static void model_free(struct ai_model *m)
{
    if (m->L)
        for (int i = 0; i < m->layers; i++) {
            struct ai_layer *l = &m->L[i];
            kfree(l->attn_norm);
            kfree(l->ffn_norm);
            kfree(l->bq);
            kfree(l->bk);
            kfree(l->bv);
            kfree(l->q_norm);
            kfree(l->k_norm);
        }
    kfree(m->L);
    kfree(m->out_norm);
    kfree(m->inv_freq);
    big_free(m->kc);
    big_free(m->vc);
    kfree(m->past);
    kfree(m->x);
    kfree(m->xb);
    kfree(m->q);
    kfree(m->k);
    kfree(m->v);
    kfree(m->att);
    kfree(m->hb);
    kfree(m->hb2);
    kfree(m->scores);
    kfree(m->logits);
    kfree(m->rope_cs);
    tok_free(&m->tok);
    gguf_free(&m->g);
    big_free(m->file);
    memset(m, 0, sizeof(*m));
}

static bool get_matrix(struct ai_model *m, const char *name, int cols, int rows, struct ai_tensor *t, char *why,
                       size_t cap)
{
    const struct gguf_tensor *g = gguf_tensor(&m->g, name);
    if (!g || g->ne[0] != (uint64_t)cols || g->ne[1] != (uint64_t)rows || g->ne[2] != 1) {
        ksnprintf(why, cap, g ? "tensor %s has an unexpected shape" : "the model has no tensor %s", name);
        return false;
    }
    int bytes = 0, per = gguf_block(g->type, &bytes);
    *t = (struct ai_tensor){g->data, g->type, cols, rows, (size_t)(cols / per * bytes)};
    m->params += (uint64_t)cols * (uint64_t)rows;
    return true;
}

/* a vector as floats (NULL and no error if optional and absent) */
static float *get_vector(struct ai_model *m, const char *name, int n, bool optional, char *why, size_t cap)
{
    const struct gguf_tensor *g = gguf_tensor(&m->g, name);
    if (!g) {
        if (!optional)
            ksnprintf(why, cap, "the model has no tensor %s", name);
        return NULL;
    }
    if (g->ne[0] != (uint64_t)n || g->ne[1] != 1) {
        ksnprintf(why, cap, "tensor %s has an unexpected shape", name);
        return NULL;
    }
    float *v = kmalloc(sizeof(float) * (size_t)n);
    if (v)
        deq_row(g->type, g->data, v, n);
    m->params += (uint64_t)n;
    return v;
}

/* takes over file (the whole .gguf in memory) */
static int model_load(struct ai_model *m, uint8_t *file, size_t size, int ctx_want, char *why, size_t cap)
{
    qdot_choose();
    memset(m, 0, sizeof(*m));
    if (!half_init()) {
        str_copy(why, "not enough memory", cap);
        return E_NOMEM;
    }
    m->file = file;
    m->file_size = size;
    int err = gguf_parse(&m->g, file, size, why, cap);
    if (err) {
        m->file = NULL;
        return err;
    }
    struct gguf_str arch = gv_str(gguf_find(&m->g, "general.architecture"));
    ksnprintf(m->arch_name, sizeof(m->arch_name), "%.*s", (int)MIN(arch.n, 15), arch.s);
    if (gs_eq(arch, "llama"))
        m->arch = ARCH_LLAMA;
    else if (gs_eq(arch, "qwen2"))
        m->arch = ARCH_QWEN2;
    else if (gs_eq(arch, "qwen3"))
        m->arch = ARCH_QWEN3;
    else {
        ksnprintf(why, cap, "models of type \"%s\" are not supported (llama, qwen2 and qwen3 are)", m->arch_name);
        m->file = NULL;
        model_free(m);
        return E_NOSYS;
    }
    struct gguf_str name = gv_str(gguf_find(&m->g, "general.name"));
    ksnprintf(m->name, sizeof(m->name), "%.*s", (int)MIN(name.n, 63), name.s);
    const char *a = m->arch_name;
    m->dim = (int)gv_uint(gguf_find_arch(&m->g, a, "embedding_length"), 0);
    m->ffn = (int)gv_uint(gguf_find_arch(&m->g, a, "feed_forward_length"), 0);
    m->layers = (int)gv_uint(gguf_find_arch(&m->g, a, "block_count"), 0);
    m->heads = (int)gv_uint(gguf_find_arch(&m->g, a, "attention.head_count"), 0);
    m->kv_heads = (int)gv_uint(gguf_find_arch(&m->g, a, "attention.head_count_kv"), (uint64_t)m->heads);
    m->hd = m->heads ? (int)gv_uint(gguf_find_arch(&m->g, a, "attention.key_length"),
                                    (uint64_t)(m->dim / m->heads))
                     : 0;
    m->eps = gv_float(gguf_find_arch(&m->g, a, "attention.layer_norm_rms_epsilon"), 1e-5f);
    float base = gv_float(gguf_find_arch(&m->g, a, "rope.freq_base"), 10000.0f);
    int train_ctx = (int)gv_uint(gguf_find_arch(&m->g, a, "context_length"), 2048);
    if (m->dim <= 0 || m->dim > 16384 || m->ffn <= 0 || m->ffn > 65536 || m->layers <= 0 || m->layers > 256 ||
        m->heads <= 0 || m->heads > 256 || m->kv_heads <= 0 || m->heads % m->kv_heads || m->hd <= 0 ||
        m->hd > 512 || (m->hd & 1)) {
        str_copy(why, "the model's dimensions make no sense", cap);
        m->file = NULL;
        model_free(m);
        return E_INVAL;
    }
    err = tok_load(&m->tok, &m->g, why, cap);
    if (err) {
        m->file = NULL;
        model_free(m);
        return err;
    }
    m->vocab = m->tok.n;
    m->ctx = MIN(ctx_want, train_ctx);
    int qd = m->heads * m->hd, kvd = m->kv_heads * m->hd;
    bool ok = get_matrix(m, "token_embd.weight", m->dim, m->vocab, &m->embd, why, cap);
    if (ok && gguf_tensor(&m->g, "output.weight"))
        ok = get_matrix(m, "output.weight", m->dim, m->vocab, &m->out, why, cap);
    else
        m->out = m->embd; /* tied embeddings */
    m->L = kzalloc(sizeof(struct ai_layer) * (size_t)m->layers);
    ok = ok && m->L && (m->out_norm = get_vector(m, "output_norm.weight", m->dim, false, why, cap));
    for (int i = 0; ok && i < m->layers; i++) {
        struct ai_layer *l = &m->L[i];
        char n[64];
#define MAT(field, part, c, r) (ksnprintf(n, sizeof(n), "blk.%d." part ".weight", i), get_matrix(m, n, c, r, &l->field, why, cap))
#define VEC(field, part, len, opt) (ksnprintf(n, sizeof(n), "blk.%d." part, i), (l->field = get_vector(m, n, len, opt, why, cap)) || opt)
        ok = MAT(q, "attn_q", m->dim, qd) && MAT(k, "attn_k", m->dim, kvd) && MAT(v, "attn_v", m->dim, kvd) &&
             MAT(o, "attn_output", qd, m->dim) && MAT(gate, "ffn_gate", m->dim, m->ffn) &&
             MAT(up, "ffn_up", m->dim, m->ffn) && MAT(down, "ffn_down", m->ffn, m->dim) &&
             VEC(attn_norm, "attn_norm.weight", m->dim, false) && VEC(ffn_norm, "ffn_norm.weight", m->dim, false) &&
             VEC(bq, "attn_q.bias", qd, true) && VEC(bk, "attn_k.bias", kvd, true) &&
             VEC(bv, "attn_v.bias", kvd, true) && VEC(q_norm, "attn_q_norm.weight", m->hd, true) &&
             VEC(k_norm, "attn_k_norm.weight", m->hd, true);
#undef MAT
#undef VEC
    }
    if (!ok) {
        m->file = NULL;
        model_free(m);
        return E_INVAL;
    }
    size_t B = AI_BATCH, wide = (size_t)MAX(MAX(qd, m->dim), kvd);
    m->inv_freq = kmalloc(sizeof(float) * (size_t)(m->hd / 2));
    m->kc = big_alloc(sizeof(float) * (size_t)m->layers * (size_t)m->ctx * (size_t)kvd);
    m->vc = big_alloc(sizeof(float) * (size_t)m->layers * (size_t)m->ctx * (size_t)kvd);
    m->past = kmalloc(sizeof(int) * (size_t)m->ctx);
    m->x = kmalloc(sizeof(float) * B * (size_t)m->dim);
    m->xb = kmalloc(sizeof(float) * B * wide);
    m->q = kmalloc(sizeof(float) * B * (size_t)qd);
    m->k = kmalloc(sizeof(float) * B * (size_t)kvd);
    m->v = kmalloc(sizeof(float) * B * (size_t)kvd);
    m->att = kmalloc(sizeof(float) * B * (size_t)qd);
    m->hb = kmalloc(sizeof(float) * B * (size_t)m->ffn);
    m->hb2 = kmalloc(sizeof(float) * B * (size_t)m->ffn);
    m->scores = kmalloc(sizeof(float) * B * (size_t)m->heads * (size_t)m->ctx);
    m->logits = kmalloc(sizeof(float) * (size_t)m->vocab);
    m->rope_cs = kmalloc(sizeof(float) * B * (size_t)m->hd);
    if (!m->inv_freq || !m->kc || !m->vc || !m->past || !m->x || !m->xb || !m->q || !m->k || !m->v || !m->att ||
        !m->hb || !m->hb2 || !m->scores || !m->logits || !m->rope_cs) {
        str_copy(why, "not enough memory for the model's working space; try a shorter context", cap);
        m->file = NULL;
        model_free(m);
        return E_NOMEM;
    }
    for (int i = 0; i < m->hd / 2; i++)
        m->inv_freq[i] = (float)ai_exp(-2.0 * i / m->hd * ai_log(base));
    return 0;
}

/* with the longest context that fits in memory, down to 512 tokens */
static int model_load_fit(struct ai_model *m, uint8_t *file, size_t size, int ctx_want, char *why, size_t cap)
{
    int err;
    while ((err = model_load(m, file, size, ctx_want, why, cap)) == E_NOMEM && ctx_want > 512)
        ctx_want /= 2;
    return err;
}

/* ---- the pieces of a step ---- */

static void rmsnorm(float *out, const float *x, const float *w, int n, float eps)
{
    float ss = 0;
    for (int i = 0; i < n; i++)
        ss += x[i] * x[i];
    float s = 1.0f / ai_sqrtf(ss / (float)n + eps);
    for (int i = 0; i < n; i++)
        out[i] = x[i] * s * w[i];
}

struct mm_job {
    const struct ai_tensor *w;
    const float *in;
    float *out;
    int batch, in_stride, out_stride;
    const int8_t *aq; /* the input in 8 bits (the integer path), else NULL */
    const float *ad;
    const int32_t *as;
};

static bool qdot_on = true; /* ai exact turns it off, to compare */
static int8_t *act_q;
static float *act_d;
static int32_t *act_s;
static size_t act_cap; /* values */

static bool act_room(size_t n)
{
    if (n <= act_cap)
        return true;
    kfree(act_q);
    kfree(act_d);
    kfree(act_s);
    act_q = kmalloc(n);
    act_d = kmalloc(sizeof(float) * (n / 32 + 1));
    act_s = kmalloc(sizeof(int32_t) * (n / 16 + 1));
    act_cap = act_q && act_d && act_s ? n : 0;
    return act_cap != 0;
}

static void mm_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit"); /* a helper processor's x87 as the first one has it */
#endif
    const struct mm_job *j = ctx;
    const struct ai_tensor *w = j->w;
    int bytes = 0, per = gguf_block(w->type, &bytes);
    float tmp[256], acc[AI_BATCH];
    for (int r = from; r < to; r++) {
        const uint8_t *row = w->data + (size_t)r * w->row_bytes;
        for (int t = 0; t < j->batch; t++)
            acc[t] = 0;
        if (j->aq) {
            int8_t qb[256];
            float sc[16], mn[16];
            for (int c = 0; c < w->cols; c += per, row += bytes) {
                int cnt = unpack_block(w->type, row, qb, sc, mn);
                for (int t = 0; t < j->batch; t++) {
                    size_t at = (size_t)t * (size_t)w->cols + (size_t)c;
                    acc[t] += qdot(qb, sc, mn, cnt, j->aq + at, j->ad + at / 32, j->as + at / 16);
                }
            }
        } else if (w->type == GT_F32) {
            for (int t = 0; t < j->batch; t++)
                acc[t] = dot((const float *)row, j->in + (size_t)t * (size_t)j->in_stride, w->cols);
        } else if (per == 1) {
            for (int c = 0; c < w->cols; c += 256) {
                int n = MIN(256, w->cols - c);
                for (int i = 0; i < n; i++)
                    tmp[i] = w->type == GT_F16 ? h2f(row + (size_t)(c + i) * 2) : bf2f(row + (size_t)(c + i) * 2);
                for (int t = 0; t < j->batch; t++)
                    acc[t] += dot(tmp, j->in + (size_t)t * (size_t)j->in_stride + c, n);
            }
        } else {
            for (int c = 0; c < w->cols; c += per, row += bytes) {
                deq_block(w->type, row, tmp);
                for (int t = 0; t < j->batch; t++)
                    acc[t] += dot(tmp, j->in + (size_t)t * (size_t)j->in_stride + c, per);
            }
        }
        for (int t = 0; t < j->batch; t++)
            j->out[(size_t)t * (size_t)j->out_stride + (size_t)r] = acc[t];
    }
}

/* out[t][r] = W[r] . in[t] for every row r and token t */
static void matmul(const struct ai_tensor *w, const float *in, int in_stride, float *out, int out_stride, int batch)
{
    struct mm_job j = {w, in, out, batch, in_stride, out_stride, NULL, NULL, NULL};
    if (qdot_on && int_type(w->type) && !(w->cols % 32) && act_room((size_t)batch * (size_t)w->cols)) {
        for (int t = 0; t < batch; t++) {
            size_t at = (size_t)t * (size_t)w->cols;
            quant_act(in + (size_t)t * (size_t)in_stride, w->cols, act_q + at, act_d + at / 32, act_s + at / 16);
        }
        j.aq = act_q;
        j.ad = act_d;
        j.as = act_s;
    }
    hal_parallel(mm_part, &j, w->rows, MAX(1, 64 / batch));
}

static void rope(float *v, const float *cs, int hd, bool neox)
{
    for (int i = 0; i < hd / 2; i++) {
        int a = neox ? i : 2 * i, b = neox ? i + hd / 2 : 2 * i + 1;
        float c = cs[2 * i], s = cs[2 * i + 1], x0 = v[a], x1 = v[b];
        v[a] = x0 * c - x1 * s;
        v[b] = x0 * s + x1 * c;
    }
}

struct att_job {
    struct ai_model *m;
    int layer, pos0, batch;
};

static void att_part(void *ctx, int from, int to)
{
#ifndef __x86_64__
    __asm__ volatile("fninit");
#endif
    const struct att_job *j = ctx;
    struct ai_model *m = j->m;
    int hd = m->hd, kvd = m->kv_heads * hd, qd = m->heads * hd, group = m->heads / m->kv_heads;
    const float *kc = m->kc + (size_t)j->layer * (size_t)m->ctx * (size_t)kvd;
    const float *vc = m->vc + (size_t)j->layer * (size_t)m->ctx * (size_t)kvd;
    float scale = 1.0f / ai_sqrtf((float)hd);
    for (int item = from; item < to; item++) {
        int t = item / m->heads, h = item % m->heads, g = h / group, len = j->pos0 + t + 1;
        const float *q = m->q + (size_t)t * (size_t)qd + (size_t)h * (size_t)hd;
        float *sc = m->scores + (size_t)item * (size_t)m->ctx, best = -1e30f;
        for (int p = 0; p < len; p++) {
            sc[p] = dot(q, kc + (size_t)p * (size_t)kvd + (size_t)g * (size_t)hd, hd) * scale;
            if (sc[p] > best)
                best = sc[p];
        }
        float sum = 0;
        for (int p = 0; p < len; p++)
            sum += sc[p] = ai_expf(sc[p] - best);
        float *o = m->att + (size_t)t * (size_t)qd + (size_t)h * (size_t)hd;
        for (int i = 0; i < hd; i++)
            o[i] = 0;
        for (int p = 0; p < len; p++) {
            float w = sc[p] / sum;
            const float *v = vc + (size_t)p * (size_t)kvd + (size_t)g * (size_t)hd;
            for (int i = 0; i < hd; i++)
                o[i] += w * v[i];
        }
    }
}

/* runs batch tokens at positions n_past..; with logits, m->logits holds the scores after the last one */
static void forward(struct ai_model *m, const int *tokens, int batch, bool logits)
{
    int dim = m->dim, hd = m->hd, qd = m->heads * hd, kvd = m->kv_heads * hd, pos0 = m->n_past;
    bool neox = m->arch != ARCH_LLAMA;
    for (int t = 0; t < batch; t++) {
        int id = tokens[t] >= 0 && tokens[t] < m->vocab ? tokens[t] : 0;
        deq_row(m->embd.type, m->embd.data + (size_t)id * m->embd.row_bytes, m->x + (size_t)t * (size_t)dim, dim);
        float *cs = m->rope_cs + (size_t)t * (size_t)hd;
        for (int i = 0; i < hd / 2; i++) {
            double s, c;
            ai_sincos((double)(pos0 + t) * (double)m->inv_freq[i], &s, &c);
            cs[2 * i] = (float)c;
            cs[2 * i + 1] = (float)s;
        }
        m->past[pos0 + t] = tokens[t];
    }
    for (int li = 0; li < m->layers; li++) {
        struct ai_layer *l = &m->L[li];
        for (int t = 0; t < batch; t++)
            rmsnorm(m->xb + (size_t)t * (size_t)dim, m->x + (size_t)t * (size_t)dim, l->attn_norm, dim, m->eps);
        matmul(&l->q, m->xb, dim, m->q, qd, batch);
        matmul(&l->k, m->xb, dim, m->k, kvd, batch);
        matmul(&l->v, m->xb, dim, m->v, kvd, batch);
        for (int t = 0; t < batch; t++) {
            float *q = m->q + (size_t)t * (size_t)qd, *k = m->k + (size_t)t * (size_t)kvd,
                  *v = m->v + (size_t)t * (size_t)kvd;
            for (int i = 0; l->bq && i < qd; i++)
                q[i] += l->bq[i];
            for (int i = 0; l->bk && i < kvd; i++)
                k[i] += l->bk[i];
            for (int i = 0; l->bv && i < kvd; i++)
                v[i] += l->bv[i];
            const float *cs = m->rope_cs + (size_t)t * (size_t)hd;
            for (int h = 0; h < m->heads; h++) {
                if (l->q_norm)
                    rmsnorm(q + h * hd, q + h * hd, l->q_norm, hd, m->eps);
                rope(q + h * hd, cs, hd, neox);
            }
            for (int h = 0; h < m->kv_heads; h++) {
                if (l->k_norm)
                    rmsnorm(k + h * hd, k + h * hd, l->k_norm, hd, m->eps);
                rope(k + h * hd, cs, hd, neox);
            }
            size_t at = ((size_t)li * (size_t)m->ctx + (size_t)(pos0 + t)) * (size_t)kvd;
            memcpy(m->kc + at, k, sizeof(float) * (size_t)kvd);
            memcpy(m->vc + at, v, sizeof(float) * (size_t)kvd);
        }
        struct att_job aj = {m, li, pos0, batch};
        hal_parallel(att_part, &aj, batch * m->heads, 1);
        matmul(&l->o, m->att, qd, m->xb, dim, batch);
        for (size_t i = 0; i < (size_t)batch * (size_t)dim; i++)
            m->x[i] += m->xb[i];
        for (int t = 0; t < batch; t++)
            rmsnorm(m->xb + (size_t)t * (size_t)dim, m->x + (size_t)t * (size_t)dim, l->ffn_norm, dim, m->eps);
        matmul(&l->gate, m->xb, dim, m->hb, m->ffn, batch);
        matmul(&l->up, m->xb, dim, m->hb2, m->ffn, batch);
        for (size_t i = 0; i < (size_t)batch * (size_t)m->ffn; i++) {
            float g = m->hb[i];
            m->hb[i] = g / (1.0f + ai_expf(-g)) * m->hb2[i];
        }
        matmul(&l->down, m->hb, m->ffn, m->xb, dim, batch);
        for (size_t i = 0; i < (size_t)batch * (size_t)dim; i++)
            m->x[i] += m->xb[i];
    }
    m->n_past += batch;
    if (logits) {
        rmsnorm(m->xb, m->x + (size_t)(batch - 1) * (size_t)dim, m->out_norm, dim, m->eps);
        matmul(&m->out, m->xb, dim, m->logits, m->vocab, 1);
    }
}

/* feeds tokens after what the cache already holds; keeps the part of the cache that matches */
static int model_feed(struct ai_model *m, const int *tokens, int n, bool (*cancel)(void))
{
    int keep = 0;
    while (keep < m->n_past && keep < n - 1 && m->past[keep] == tokens[keep])
        keep++;
    m->n_past = keep;
    if (n > m->ctx)
        return E_RANGE;
    for (int i = keep; i < n; i += AI_BATCH) {
        if (cancel && cancel())
            return E_INTR;
        int b = MIN(AI_BATCH, n - i);
        forward(m, tokens + i, b, i + b == n);
    }
    return 0;
}

/* ---- choosing the next token ---- */

struct sampler {
    float temp, top_p, repeat;
    uint64_t rng;
};

static uint32_t rng_next(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return (uint32_t)(*s >> 16);
}

static int sample(struct ai_model *m, struct sampler *s)
{
    float *lg = m->logits;
    int n = m->vocab;
    if (s->repeat > 1.0f) /* recent tokens a little less likely: small models loop otherwise */
        for (int i = MAX(0, m->n_past - 64); i < m->n_past; i++) {
            int id = m->past[i];
            lg[id] = lg[id] > 0 ? lg[id] / s->repeat : lg[id] * s->repeat;
        }
    int best = 0;
    for (int i = 1; i < n; i++)
        if (lg[i] > lg[best])
            best = i;
    if (s->temp <= 0)
        return best;
    enum { K = 40 };
    int top[K], count = 0;
    for (int i = 0; i < n; i++) {
        if (count == K && lg[i] <= lg[top[K - 1]])
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
    float cut = 0;
    int keep = 0;
    while (keep < count && (cut < s->top_p * sum || !keep))
        cut += p[keep++];
    float r = (float)(rng_next(&s->rng) % 1000000u) / 1000000.0f * cut;
    for (int i = 0; i < keep; i++) {
        r -= p[i];
        if (r <= 0)
            return top[i];
    }
    return top[keep - 1];
}
