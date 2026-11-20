/* The language model: a decoder-only transformer of the Llama family as llama.cpp stores it: RMSNorm,
   grouped-query attention with rotary positions, a SwiGLU feed-forward block. The architectures differ in small
   ways, each a field of struct ai_model: llama and mistral (rotation of neighbours), qwen2 (biases), qwen3 (norms of
   queries and keys), starcoder2 (layer norms, biases, GELU), minicpm (scaled embeddings, residuals and logits), phi3
   (one matrix for queries, keys and values, one for gate and up, partial rotation, rope factors), gemma (scaled
   embeddings, a GELU gate), gemma2 (norms after attention and feed-forward, soft caps on scores and logits, every
   other layer attends to a window) and gemma3 (norms of queries and keys, five of six layers attend to a window and
   rotate with their own base).
   Weights stay quantised in the loaded file; each matrix row is turned into floats block by block while it is
   multiplied, rows are shared out among the processors (hal_parallel). Prompts go through in batches of up to
   AI_BATCH tokens so each row is decoded once per batch. Keys and values of earlier tokens stay in a cache. */

#define AI_BATCH 16

enum { ARCH_LLAMA, ARCH_QWEN2, ARCH_QWEN3, ARCH_STARCODER2, ARCH_MINICPM, ARCH_PHI3, ARCH_GEMMA, ARCH_GEMMA2,
       ARCH_GEMMA3 };

struct ai_tensor {
    const uint8_t *data;
    uint32_t type;
    int cols, rows;
    size_t row_bytes;
    void *dev; /* the copy a compute backend keeps, or NULL */
};

/* a GPU or NPU module's backend (synth/aibackend.h), looked up when a model is loaded; NULL: the processors */
static const struct ai_backend *ai_dev;

static void ai_dev_find(void)
{
    ai_dev = module_service ? module_service("ai-backend") : NULL;
    if (ai_dev && (!ai_dev->upload || !ai_dev->matmul))
        ai_dev = NULL;
}

static void tensor_release(struct ai_tensor *t)
{
    if (t->dev && ai_dev && ai_dev->release)
        ai_dev->release(t->dev);
    t->dev = NULL;
}

struct ai_layer {
    struct ai_tensor q, k, v, o, gate, up, down;
    struct ai_tensor qkv;                   /* Phi-3: queries, keys and values in one (q, k, v then unused) */
    float *post_attn_norm, *post_ffn_norm; /* Gemma 2 and 3 */
    float *attn_norm, *ffn_norm, *bq, *bk, *bv, *q_norm, *k_norm;
    float *attn_norm_b, *ffn_norm_b, *bo, *bup, *bdown; /* StarCoder2: layer norms and biases everywhere */
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
    float *out_norm, *out_norm_b;
    struct ai_layer *L;
    float *inv_freq;
    /* the differences between architectures; 0 means "not used" */
    int n_rot;                   /* the dimensions of a head that rotate (Phi-4 mini: 96 of 128) */
    float emb_scale, res_scale;  /* MiniCPM, Gemma: the input embeddings times emb_scale; residuals times res_scale */
    float logit_div;             /* MiniCPM: logits divided by this */
    float att_scale;             /* scores times this (1/sqrt(head size) unless the model says otherwise) */
    float att_cap, final_cap;    /* Gemma 2: scores and logits squashed by c * tanh(x / c) */
    int swa, swa_pattern;        /* Gemma 2, 3: of every swa_pattern layers all but the last see only swa tokens */
    bool gelu_gate, fused;       /* GELU on the gate (Gemma); Phi-3's joined matrices */
    float *inv_freq_local;       /* Gemma 3: the window layers' own rotation */
    float *rope_cs_local, *qkv;
    uint16_t *kc, *vc; /* [layer][ctx][kv_heads * hd] in F16: half the memory of floats, and half the reading */
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
            struct ai_tensor *ts[] = {&l->q, &l->k, &l->v, &l->o, &l->gate, &l->up, &l->down, &l->qkv};
            for (size_t k = 0; k < ARRAY_SIZE(ts); k++)
                tensor_release(ts[k]);
            kfree(l->attn_norm);
            kfree(l->ffn_norm);
            kfree(l->bq);
            kfree(l->bk);
            kfree(l->bv);
            kfree(l->q_norm);
            kfree(l->k_norm);
            kfree(l->attn_norm_b);
            kfree(l->ffn_norm_b);
            kfree(l->bo);
            kfree(l->bup);
            kfree(l->bdown);
            kfree(l->post_attn_norm);
            kfree(l->post_ffn_norm);
        }
    kfree(m->L);
    if (m->out.dev != m->embd.dev)
        tensor_release(&m->out);
    tensor_release(&m->embd);
    kfree(m->out_norm);
    kfree(m->out_norm_b);
    kfree(m->inv_freq);
    kfree(m->inv_freq_local);
    kfree(m->rope_cs_local);
    kfree(m->qkv);
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
    *t = (struct ai_tensor){g->data, g->type, cols, rows, (size_t)(cols / per * bytes), NULL};
    m->params += (uint64_t)cols * (uint64_t)rows;
    if (ai_dev)
        t->dev = ai_dev->upload(t->data, t->type, cols, rows, t->row_bytes);
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

/* the layers: prefix "blk.N." (or e.g. "talker.blk.N."), the final norm; dimensions already set */
static int model_blocks(struct ai_model *m, const char *prefix, char *why, size_t cap)
{
    int qd = m->heads * m->hd, kvd = m->kv_heads * m->hd;
    m->L = kzalloc(sizeof(struct ai_layer) * (size_t)m->layers);
    bool sc2 = m->arch == ARCH_STARCODER2, ok = m->L != NULL;
    char n[80];
    ksnprintf(n, sizeof(n), "%soutput_norm.weight", prefix);
    ok = ok && (m->out_norm = get_vector(m, n, m->dim, false, why, cap));
    ksnprintf(n, sizeof(n), "%soutput_norm.bias", prefix);
    if (ok && sc2)
        ok = (m->out_norm_b = get_vector(m, n, m->dim, false, why, cap)) != NULL;
    for (int i = 0; ok && i < m->layers; i++) {
        struct ai_layer *l = &m->L[i];
#define MAT(field, part, c, r) (ksnprintf(n, sizeof(n), "%sblk.%d." part ".weight", prefix, i), get_matrix(m, n, c, r, &l->field, why, cap))
#define VEC(field, part, len, opt) (ksnprintf(n, sizeof(n), "%sblk.%d." part, prefix, i), (l->field = get_vector(m, n, len, opt, why, cap)) || opt)
        if (m->fused)
            ok = MAT(qkv, "attn_qkv", m->dim, qd + 2 * kvd) && MAT(up, "ffn_up", m->dim, 2 * m->ffn);
        else
            ok = MAT(q, "attn_q", m->dim, qd) && MAT(k, "attn_k", m->dim, kvd) && MAT(v, "attn_v", m->dim, kvd) &&
                 (sc2 || MAT(gate, "ffn_gate", m->dim, m->ffn)) && MAT(up, "ffn_up", m->dim, m->ffn);
        ok = ok && MAT(o, "attn_output", qd, m->dim) && MAT(down, "ffn_down", m->ffn, m->dim) &&
             VEC(attn_norm, "attn_norm.weight", m->dim, false) && VEC(ffn_norm, "ffn_norm.weight", m->dim, false) &&
             VEC(bq, "attn_q.bias", qd, true) && VEC(bk, "attn_k.bias", kvd, true) &&
             VEC(bv, "attn_v.bias", kvd, true) && VEC(q_norm, "attn_q_norm.weight", m->hd, true) &&
             VEC(k_norm, "attn_k_norm.weight", m->hd, true) && VEC(attn_norm_b, "attn_norm.bias", m->dim, !sc2) &&
             VEC(ffn_norm_b, "ffn_norm.bias", m->dim, !sc2) && VEC(bo, "attn_output.bias", m->dim, true) &&
             VEC(bup, "ffn_up.bias", m->ffn, true) && VEC(bdown, "ffn_down.bias", m->dim, true) &&
             VEC(post_attn_norm, "post_attention_norm.weight", m->dim, true) &&
             VEC(post_ffn_norm, "post_ffw_norm.weight", m->dim, true);
#undef MAT
#undef VEC
    }
    return ok ? 0 : E_INVAL;
}

/* the key and value cache for m->ctx tokens and the working space of a step */
static int model_work(struct ai_model *m, float base, char *why, size_t cap)
{
    int qd = m->heads * m->hd, kvd = m->kv_heads * m->hd;
    size_t B = AI_BATCH, wide = (size_t)MAX(MAX(qd, m->dim), kvd);
    if (!m->n_rot)
        m->n_rot = m->hd;
    if (!m->att_scale)
        m->att_scale = 1.0f / ai_sqrtf((float)m->hd);
    m->inv_freq = kmalloc(sizeof(float) * (size_t)(m->hd / 2));
    m->kc = big_alloc(sizeof(uint16_t) * (size_t)m->layers * (size_t)m->ctx * (size_t)kvd);
    m->vc = big_alloc(sizeof(uint16_t) * (size_t)m->layers * (size_t)m->ctx * (size_t)kvd);
    m->past = kmalloc(sizeof(int) * (size_t)m->ctx);
    m->x = kmalloc(sizeof(float) * B * (size_t)m->dim);
    m->xb = kmalloc(sizeof(float) * B * wide);
    m->q = kmalloc(sizeof(float) * B * (size_t)qd);
    m->k = kmalloc(sizeof(float) * B * (size_t)kvd);
    m->v = kmalloc(sizeof(float) * B * (size_t)kvd);
    m->att = kmalloc(sizeof(float) * B * (size_t)qd);
    m->hb = kmalloc(sizeof(float) * B * (size_t)m->ffn);
    m->hb2 = kmalloc(sizeof(float) * B * (size_t)m->ffn * (m->fused ? 2 : 1)); /* Phi-3: gate and up */
    if (m->fused)
        m->qkv = kmalloc(sizeof(float) * B * (size_t)(qd + 2 * kvd));
    m->scores = kmalloc(sizeof(float) * B * (size_t)m->heads * (size_t)m->ctx);
    m->logits = kmalloc(sizeof(float) * (size_t)m->vocab);
    m->rope_cs = kmalloc(sizeof(float) * B * (size_t)m->hd);
    if (!m->inv_freq || !m->kc || !m->vc || !m->past || !m->x || !m->xb || !m->q || !m->k || !m->v || !m->att ||
        !m->hb || !m->hb2 || !m->scores || !m->logits || !m->rope_cs || (m->fused && !m->qkv)) {
        str_copy(why, "not enough memory for the model's working space; try a shorter context", cap);
        return E_NOMEM;
    }
    for (int i = 0; i < m->n_rot / 2; i++)
        m->inv_freq[i] = (float)ai_exp(-2.0 * i / m->n_rot * ai_log(base));
    return 0;
}

/* Gemma 3: the window layers rotate with their own base */
static int model_local_rope(struct ai_model *m, float base)
{
    m->inv_freq_local = kmalloc(sizeof(float) * (size_t)(m->hd / 2));
    m->rope_cs_local = kmalloc(sizeof(float) * AI_BATCH * (size_t)m->hd);
    if (!m->inv_freq_local || !m->rope_cs_local)
        return E_NOMEM;
    for (int i = 0; i < m->n_rot / 2; i++)
        m->inv_freq_local[i] = (float)ai_exp(-2.0 * i / m->n_rot * ai_log(base));
    return 0;
}

static float arch_float(struct ai_model *m, const char *key, float fallback)
{
    return gv_float(gguf_find_arch(&m->g, m->arch_name, key), fallback);
}

/* what sets an architecture apart, from its name and metadata; before model_blocks */
static void model_traits(struct ai_model *m)
{
    m->n_rot = (int)gv_uint(gguf_find_arch(&m->g, m->arch_name, "rope.dimension_count"), (uint64_t)m->hd);
    if (m->n_rot <= 0 || m->n_rot > m->hd || (m->n_rot & 1))
        m->n_rot = m->hd;
    switch (m->arch) {
    case ARCH_MINICPM: /* older files lack the keys: then MiniCPM's own constants */
        m->emb_scale = arch_float(m, "embedding_scale", 12.0f);
        m->res_scale = arch_float(m, "residual_scale", 1.4f / ai_sqrtf((float)m->layers));
        m->logit_div = arch_float(m, "logit_scale", (float)m->dim / 256.0f);
        break;
    case ARCH_PHI3:
        m->fused = true;
        break;
    case ARCH_GEMMA3:
        m->swa_pattern = 6;
        m->swa = (int)gv_uint(gguf_find_arch(&m->g, m->arch_name, "attention.sliding_window"), 0);
        /* fall through */
    case ARCH_GEMMA2:
        if (m->arch == ARCH_GEMMA2) {
            m->swa_pattern = 2;
            m->swa = (int)gv_uint(gguf_find_arch(&m->g, m->arch_name, "attention.sliding_window"), 0);
            m->att_cap = arch_float(m, "attn_logit_softcapping", 0.0f);
            m->final_cap = arch_float(m, "final_logit_softcapping", 0.0f);
        }
        /* the 27B models scale queries by the width of a head as the width / heads, not the key length */
        if ((m->arch == ARCH_GEMMA2 && m->layers == 46) || (m->arch == ARCH_GEMMA3 && m->layers == 62))
            m->att_scale = 1.0f / ai_sqrtf((float)(m->dim / m->heads));
        /* fall through */
    case ARCH_GEMMA:
        m->emb_scale = ai_sqrtf((float)m->dim);
        m->gelu_gate = true;
        break;
    }
}

/* Phi-3 and others with long-context rope factors: frequencies divided by the short factors (the context here is
   short); Gemma 3's linear scaling stretches the global layers' positions */
static void model_rope_factors(struct ai_model *m)
{
    const struct gguf_tensor *f = gguf_tensor(&m->g, "rope_factors_short.weight");
    if (!f)
        f = gguf_tensor(&m->g, "rope_factors_long.weight");
    if (f && f->ne[0] == (uint64_t)(m->n_rot / 2) && f->type == GT_F32) {
        const float *v = (const float *)f->data;
        for (int i = 0; i < m->n_rot / 2; i++)
            if (v[i] > 0)
                m->inv_freq[i] /= v[i];
    }
    float lin = arch_float(m, "rope.scaling.factor", 0.0f);
    if (m->arch == ARCH_GEMMA3 && lin > 1.0f)
        for (int i = 0; i < m->n_rot / 2; i++)
            m->inv_freq[i] /= lin;
}


/* takes over file (the whole .gguf in memory) */
static int model_load(struct ai_model *m, uint8_t *file, size_t size, int ctx_want, char *why, size_t cap)
{
    qdot_choose();
    ai_dev_find();
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
    else if (gs_eq(arch, "starcoder2"))
        m->arch = ARCH_STARCODER2;
    else if (gs_eq(arch, "mistral"))
        m->arch = ARCH_LLAMA;
    else if (gs_eq(arch, "minicpm"))
        m->arch = ARCH_MINICPM;
    else if (gs_eq(arch, "phi3"))
        m->arch = ARCH_PHI3;
    else if (gs_eq(arch, "gemma"))
        m->arch = ARCH_GEMMA;
    else if (gs_eq(arch, "gemma2"))
        m->arch = ARCH_GEMMA2;
    else if (gs_eq(arch, "gemma3"))
        m->arch = ARCH_GEMMA3;
    else {
        ksnprintf(why, cap,
                  "models of type \"%s\" are not supported (llama, mistral, qwen2, qwen3, starcoder2, minicpm, "
                  "phi3, gemma, gemma2 and gemma3 are)",
                  m->arch_name);
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
    m->eps = gv_float(gguf_find_arch(&m->g, a, m->arch == ARCH_STARCODER2 ? "attention.layer_norm_epsilon"
                                                                          : "attention.layer_norm_rms_epsilon"),
                      1e-5f);
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
    bool ok = get_matrix(m, "token_embd.weight", m->dim, m->vocab, &m->embd, why, cap);
    if (ok && gguf_tensor(&m->g, "output.weight"))
        ok = get_matrix(m, "output.weight", m->dim, m->vocab, &m->out, why, cap);
    else
        m->out = m->embd; /* tied embeddings */
    model_traits(m);
    if (ok && (err = model_blocks(m, "", why, cap)) == 0 && (err = model_work(m, base, why, cap)) == 0) {
        model_rope_factors(m);
        if (m->arch == ARCH_GEMMA3 && m->swa && model_local_rope(m, arch_float(m, "rope.local.freq_base", 10000.0f)))
            err = E_NOMEM;
    }
    if (!ok || err) {
        m->file = NULL;
        model_free(m);
        return ok ? err : E_INVAL;
    }
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

/* the model's normalisation: RMS norm, or for StarCoder2 a layer norm with a bias */
static void norm(const struct ai_model *m, float *out, const float *x, const float *w, const float *b, int n)
{
    if (!b) {
        rmsnorm(out, x, w, n, m->eps);
        return;
    }
    float mean = 0, var = 0;
    for (int i = 0; i < n; i++)
        mean += x[i];
    mean /= (float)n;
    for (int i = 0; i < n; i++)
        var += (x[i] - mean) * (x[i] - mean);
    float k = 1.0f / ai_sqrtf(var / (float)n + m->eps);
    for (int i = 0; i < n; i++)
        out[i] = (x[i] - mean) * k * w[i] + b[i];
}

static void add_bias(float *x, const float *b, int n, int batch, int stride)
{
    for (int t = 0; b && t < batch; t++)
        for (int i = 0; i < n; i++)
            x[(size_t)t * (size_t)stride + (size_t)i] += b[i];
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

#define qdot_on (gui_ai.speed == 0) /* Settings → AI → speed, or ai exact: floats everywhere, to compare */
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
        if (j->aq && j->batch <= 4 && qdot_direct(w->type)) { /* straight from the file; a batch unpacks once */
            for (int c = 0; c < w->cols; c += per, row += bytes)
                for (int t = 0; t < j->batch; t++) {
                    size_t at = (size_t)t * (size_t)w->cols + (size_t)c;
                    acc[t] += qdot_block(w->type, row, j->aq + at, j->ad + at / 32, j->as + at / 16);
                }
        } else if (j->aq) {
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
    if (w->dev && ai_dev->matmul(w->dev, in, in_stride, out, out_stride, batch))
        return;
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

static inline float ai_tanhf(float x)
{
    return x > 10.0f ? 1.0f : x < -10.0f ? -1.0f : 1.0f - 2.0f / (ai_expf(2.0f * x) + 1.0f);
}

static inline float gelu_tanh(float x)
{
    return 0.5f * x * (1.0f + ai_tanhf(0.7978845608f * (x + 0.044715f * x * x * x)));
}

/* how many tokens back a layer sees: 0 for all */
static int model_window(const struct ai_model *m, int layer)
{
    return m->swa && m->swa_pattern && layer % m->swa_pattern < m->swa_pattern - 1 ? m->swa : 0;
}

/* x += r, or r times the residual scale (MiniCPM) */
static void residual(const struct ai_model *m, float *x, const float *r, size_t n)
{
    float k = m->res_scale ? m->res_scale : 1.0f;
    for (size_t i = 0; i < n; i++)
        x[i] += k * r[i];
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
    const uint16_t *kc = m->kc + (size_t)j->layer * (size_t)m->ctx * (size_t)kvd;
    const uint16_t *vc = m->vc + (size_t)j->layer * (size_t)m->ctx * (size_t)kvd;
    float scale = m->att_scale, cap = m->att_cap;
    int window = model_window(m, j->layer);
    for (int item = from; item < to; item++) {
        int t = item / m->heads, h = item % m->heads, g = h / group, len = j->pos0 + t + 1;
        int first = window && len > window ? len - window : 0;
        const float *q = m->q + (size_t)t * (size_t)qd + (size_t)h * (size_t)hd;
        float *sc = m->scores + (size_t)item * (size_t)m->ctx, best = -1e30f;
        for (int p = first; p < len; p++) {
            sc[p] = kv_dot(q, kc + (size_t)p * (size_t)kvd + (size_t)g * (size_t)hd, hd) * scale;
            if (cap > 0)
                sc[p] = cap * ai_tanhf(sc[p] / cap);
            if (sc[p] > best)
                best = sc[p];
        }
        float sum = 0;
        for (int p = first; p < len; p++)
            sum += sc[p] = ai_expf(sc[p] - best);
        float *o = m->att + (size_t)t * (size_t)qd + (size_t)h * (size_t)hd;
        for (int i = 0; i < hd; i++)
            o[i] = 0;
        for (int p = first; p < len; p++) {
            kv_add(o, sc[p] / sum, vc + (size_t)p * (size_t)kvd + (size_t)g * (size_t)hd, hd);
        }
    }
}

/* runs batch tokens at positions n_past..; with logits, m->logits holds the scores after the last one (and m->xb the
   normalised state they come from). tokens NULL: m->x already holds the batch's input vectors. */
static void forward(struct ai_model *m, const int *tokens, int batch, bool logits)
{
    int dim = m->dim, hd = m->hd, qd = m->heads * hd, kvd = m->kv_heads * hd, pos0 = m->n_past, rot = m->n_rot;
    bool neox = m->arch != ARCH_LLAMA && m->arch != ARCH_MINICPM; /* halves rotate, not neighbours */
    for (int t = 0; t < batch; t++) {
        if (tokens) {
            int id = tokens[t] >= 0 && tokens[t] < m->vocab ? tokens[t] : 0;
            float *x = m->x + (size_t)t * (size_t)dim;
            deq_row(m->embd.type, m->embd.data + (size_t)id * m->embd.row_bytes, x, dim);
            for (int i = 0; m->emb_scale && i < dim; i++)
                x[i] *= m->emb_scale;
        }
        for (int local = 0; local < (m->rope_cs_local ? 2 : 1); local++) {
            float *cs = (local ? m->rope_cs_local : m->rope_cs) + (size_t)t * (size_t)hd;
            const float *f = local ? m->inv_freq_local : m->inv_freq;
            for (int i = 0; i < rot / 2; i++) {
                double s, c;
                ai_sincos((double)(pos0 + t) * (double)f[i], &s, &c);
                cs[2 * i] = (float)c;
                cs[2 * i + 1] = (float)s;
            }
        }
        m->past[pos0 + t] = tokens ? tokens[t] : -1;
    }
    for (int li = 0; li < m->layers; li++) {
        struct ai_layer *l = &m->L[li];
        for (int t = 0; t < batch; t++)
            norm(m, m->xb + (size_t)t * (size_t)dim, m->x + (size_t)t * (size_t)dim, l->attn_norm, l->attn_norm_b, dim);
        if (m->fused) { /* Phi-3: one product, then split */
            int all = qd + 2 * kvd;
            matmul(&l->qkv, m->xb, dim, m->qkv, all, batch);
            for (int t = 0; t < batch; t++) {
                const float *r = m->qkv + (size_t)t * (size_t)all;
                memcpy(m->q + (size_t)t * (size_t)qd, r, sizeof(float) * (size_t)qd);
                memcpy(m->k + (size_t)t * (size_t)kvd, r + qd, sizeof(float) * (size_t)kvd);
                memcpy(m->v + (size_t)t * (size_t)kvd, r + qd + kvd, sizeof(float) * (size_t)kvd);
            }
        } else {
            matmul(&l->q, m->xb, dim, m->q, qd, batch);
            matmul(&l->k, m->xb, dim, m->k, kvd, batch);
            matmul(&l->v, m->xb, dim, m->v, kvd, batch);
        }
        bool local = m->rope_cs_local && model_window(m, li);
        for (int t = 0; t < batch; t++) {
            float *q = m->q + (size_t)t * (size_t)qd, *k = m->k + (size_t)t * (size_t)kvd,
                  *v = m->v + (size_t)t * (size_t)kvd;
            for (int i = 0; l->bq && i < qd; i++)
                q[i] += l->bq[i];
            for (int i = 0; l->bk && i < kvd; i++)
                k[i] += l->bk[i];
            for (int i = 0; l->bv && i < kvd; i++)
                v[i] += l->bv[i];
            const float *cs = (local ? m->rope_cs_local : m->rope_cs) + (size_t)t * (size_t)hd;
            for (int h = 0; h < m->heads; h++) {
                if (l->q_norm)
                    rmsnorm(q + h * hd, q + h * hd, l->q_norm, hd, m->eps);
                rope(q + h * hd, cs, rot, neox);
            }
            for (int h = 0; h < m->kv_heads; h++) {
                if (l->k_norm)
                    rmsnorm(k + h * hd, k + h * hd, l->k_norm, hd, m->eps);
                rope(k + h * hd, cs, rot, neox);
            }
            size_t at = ((size_t)li * (size_t)m->ctx + (size_t)(pos0 + t)) * (size_t)kvd;
            kv_put(m->kc + at, k, kvd);
            kv_put(m->vc + at, v, kvd);
        }
        struct att_job aj = {m, li, pos0, batch};
        hal_parallel(att_part, &aj, batch * m->heads, 1);
        matmul(&l->o, m->att, qd, m->xb, dim, batch);
        add_bias(m->xb, l->bo, dim, batch, dim);
        for (int t = 0; l->post_attn_norm && t < batch; t++)
            rmsnorm(m->xb + (size_t)t * (size_t)dim, m->xb + (size_t)t * (size_t)dim, l->post_attn_norm, dim, m->eps);
        residual(m, m->x, m->xb, (size_t)batch * (size_t)dim);
        for (int t = 0; t < batch; t++)
            norm(m, m->xb + (size_t)t * (size_t)dim, m->x + (size_t)t * (size_t)dim, l->ffn_norm, l->ffn_norm_b, dim);
        if (m->fused) { /* Phi-3: gate and up from one product, the gate first */
            size_t f = (size_t)m->ffn;
            matmul(&l->up, m->xb, dim, m->hb2, (int)(2 * f), batch);
            for (size_t t = 0; t < (size_t)batch; t++)
                for (size_t i = 0; i < f; i++) {
                    float g = m->hb2[t * 2 * f + i];
                    m->hb[t * f + i] = g / (1.0f + ai_expf(-g)) * m->hb2[t * 2 * f + f + i];
                }
        } else if (!l->gate.data) { /* StarCoder2: up, GELU, down */
            matmul(&l->up, m->xb, dim, m->hb, m->ffn, batch);
            add_bias(m->hb, l->bup, m->ffn, batch, m->ffn);
            for (size_t i = 0; i < (size_t)batch * (size_t)m->ffn; i++)
                m->hb[i] = gelu_tanh(m->hb[i]);
        } else {
            matmul(&l->gate, m->xb, dim, m->hb, m->ffn, batch);
            matmul(&l->up, m->xb, dim, m->hb2, m->ffn, batch);
            for (size_t i = 0; i < (size_t)batch * (size_t)m->ffn; i++) {
                float g = m->hb[i];
                m->hb[i] = (m->gelu_gate ? gelu_tanh(g) : g / (1.0f + ai_expf(-g))) * m->hb2[i];
            }
        }
        matmul(&l->down, m->hb, m->ffn, m->xb, dim, batch);
        add_bias(m->xb, l->bdown, dim, batch, dim);
        for (int t = 0; l->post_ffn_norm && t < batch; t++)
            rmsnorm(m->xb + (size_t)t * (size_t)dim, m->xb + (size_t)t * (size_t)dim, l->post_ffn_norm, dim, m->eps);
        residual(m, m->x, m->xb, (size_t)batch * (size_t)dim);
    }
    m->n_past += batch;
    if (logits) {
        norm(m, m->xb, m->x + (size_t)(batch - 1) * (size_t)dim, m->out_norm, m->out_norm_b, dim);
        matmul(&m->out, m->xb, dim, m->logits, m->vocab, 1);
        for (int i = 0; m->logit_div && i < m->vocab; i++)
            m->logits[i] /= m->logit_div;
        for (int i = 0; m->final_cap > 0 && i < m->vocab; i++)
            m->logits[i] = m->final_cap * ai_tanhf(m->logits[i] / m->final_cap);
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
