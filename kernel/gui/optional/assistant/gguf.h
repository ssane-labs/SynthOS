/* GGUF, the file format of llama.cpp models (version 2 and 3): metadata (key, type, value) followed by tensor
   descriptions and the aligned tensor data. The whole file is in memory; every length is checked against it. */

/* Big buffers (a model file, its key and value cache) come from the memory past the kernel heap where there is
   some: the heap holds 1 GiB, a 2B model in Q4 is 1.6 GB. */
#define BIG_MIN (64u << 20)
#define BIG_SLOTS 16

static struct {
    void *p;
    size_t size;
} bigs[BIG_SLOTS];

static void *big_alloc(size_t size)
{
    if (size >= BIG_MIN)
        for (int i = 0; i < BIG_SLOTS; i++)
            if (!bigs[i].p) {
                void *p = vmm_big_alloc(size);
                if (!p)
                    break;
                bigs[i].p = p;
                bigs[i].size = size;
                return p;
            }
    return kmalloc(size ? size : 1);
}

static void big_free(void *p)
{
    if (!p)
        return;
    for (int i = 0; i < BIG_SLOTS; i++)
        if (bigs[i].p == p) {
            vmm_big_free(p, bigs[i].size);
            bigs[i].p = NULL;
            return;
        }
    kfree(p);
}

enum {
    GV_U8, GV_I8, GV_U16, GV_I16, GV_U32, GV_I32, GV_F32, GV_BOOL, GV_STR, GV_ARR, GV_U64, GV_I64, GV_F64,
};

/* ggml tensor types */
enum {
    GT_F32 = 0, GT_F16 = 1, GT_Q4_0 = 2, GT_Q4_1 = 3, GT_Q5_0 = 6, GT_Q5_1 = 7, GT_Q8_0 = 8, GT_Q2_K = 10,
    GT_Q3_K = 11, GT_Q4_K = 12, GT_Q5_K = 13, GT_Q6_K = 14, GT_BF16 = 30,
};

struct gguf_str {
    const char *s;
    size_t n;
};

struct gguf_kv {
    struct gguf_str key;
    uint32_t type;
    const uint8_t *value; /* GV_ARR: item type (u32), count (u64), items */
};

struct gguf_tensor {
    struct gguf_str name;
    uint32_t type;
    int dims;
    uint64_t ne[4];
    const uint8_t *data;
    size_t bytes;
};

struct gguf {
    const uint8_t *buf;
    size_t size;
    struct gguf_kv *kv;
    int kv_count;
    struct gguf_tensor *t;
    int t_count;
};

/* values per block and bytes per block of a tensor type; 0 when the type is not read */
static int gguf_block(uint32_t type, int *bytes)
{
    static const struct {
        uint8_t type;
        uint16_t per, bytes;
    } info[] = {{GT_F32, 1, 4},     {GT_F16, 1, 2},      {GT_BF16, 1, 2},     {GT_Q4_0, 32, 18},
                {GT_Q4_1, 32, 20},  {GT_Q5_0, 32, 22},   {GT_Q5_1, 32, 24},   {GT_Q8_0, 32, 34},
                {GT_Q2_K, 256, 84}, {GT_Q3_K, 256, 110}, {GT_Q4_K, 256, 144}, {GT_Q5_K, 256, 176},
                {GT_Q6_K, 256, 210}};
    for (size_t i = 0; i < ARRAY_SIZE(info); i++)
        if (info[i].type == type) {
            *bytes = info[i].bytes;
            return info[i].per;
        }
    return 0;
}

static const char *gguf_type_name(uint32_t type)
{
    switch (type) {
    case GT_F32: return "F32";
    case GT_F16: return "F16";
    case GT_BF16: return "BF16";
    case GT_Q4_0: return "Q4_0";
    case GT_Q4_1: return "Q4_1";
    case GT_Q5_0: return "Q5_0";
    case GT_Q5_1: return "Q5_1";
    case GT_Q8_0: return "Q8_0";
    case GT_Q2_K: return "Q2_K";
    case GT_Q3_K: return "Q3_K";
    case GT_Q4_K: return "Q4_K";
    case GT_Q5_K: return "Q5_K";
    case GT_Q6_K: return "Q6_K";
    }
    return "?";
}

struct gguf_reader {
    const uint8_t *p, *end;
    bool bad;
};

static const uint8_t *rd_take(struct gguf_reader *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *at = r->p;
    r->p += n;
    return at;
}

static uint32_t rd_u32(struct gguf_reader *r)
{
    const uint8_t *p = rd_take(r, 4);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}

static uint64_t rd_u64(struct gguf_reader *r)
{
    uint64_t lo = rd_u32(r);
    return lo | (uint64_t)rd_u32(r) << 32;
}

static struct gguf_str rd_str(struct gguf_reader *r)
{
    uint64_t n = rd_u64(r);
    const uint8_t *p = n < (1u << 30) ? rd_take(r, (size_t)n) : (r->bad = true, NULL);
    return (struct gguf_str){p ? (const char *)p : "", p ? (size_t)n : 0};
}

static size_t gv_size(uint32_t type)
{
    static const uint8_t sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    return type < ARRAY_SIZE(sizes) ? sizes[type] : 0;
}

/* steps over one value of the given type */
static void rd_skip(struct gguf_reader *r, uint32_t type, int depth)
{
    if (type == GV_STR) {
        rd_str(r);
    } else if (type == GV_ARR) {
        uint32_t sub = rd_u32(r);
        uint64_t n = rd_u64(r);
        if (depth > 2 || n > (1u << 26)) {
            r->bad = true;
            return;
        }
        if (sub != GV_STR && sub != GV_ARR) {
            size_t sz = gv_size(sub);
            if (!sz || n > (size_t)(r->end - r->p) / sz)
                r->bad = true;
            else
                r->p += (size_t)n * sz;
            return;
        }
        for (uint64_t i = 0; i < n && !r->bad; i++)
            rd_skip(r, sub, depth + 1);
    } else if (gv_size(type)) {
        rd_take(r, gv_size(type));
    } else {
        r->bad = true;
    }
}

static void gguf_free(struct gguf *g)
{
    kfree(g->kv);
    kfree(g->t);
    memset(g, 0, sizeof(*g));
}

static int gguf_parse(struct gguf *g, const uint8_t *buf, size_t size, char *why, size_t cap)
{
    memset(g, 0, sizeof(*g));
    g->buf = buf;
    g->size = size;
    struct gguf_reader r = {buf, buf + size, false};
    const uint8_t *magic = rd_take(&r, 4);
    if (!magic || memcmp(magic, "GGUF", 4)) {
        str_copy(why, "not a GGUF model", cap);
        return E_INVAL;
    }
    uint32_t version = rd_u32(&r);
    uint64_t nt = rd_u64(&r), nkv = rd_u64(&r);
    if (version < 2 || version > 3 || nt > 65536 || nkv > 65536) {
        str_copy(why, "unsupported GGUF version", cap);
        return E_INVAL;
    }
    g->kv = kzalloc(sizeof(*g->kv) * (size_t)(nkv + 1));
    g->t = kzalloc(sizeof(*g->t) * (size_t)(nt + 1));
    if (!g->kv || !g->t) {
        gguf_free(g);
        return E_NOMEM;
    }
    for (uint64_t i = 0; i < nkv && !r.bad; i++) {
        struct gguf_kv *kv = &g->kv[g->kv_count++];
        kv->key = rd_str(&r);
        kv->type = rd_u32(&r);
        kv->value = r.p;
        rd_skip(&r, kv->type, 0);
    }
    uint32_t align = 32;
    for (int i = 0; i < g->kv_count; i++)
        if (g->kv[i].key.n == 17 && !memcmp(g->kv[i].key.s, "general.alignment", 17) && g->kv[i].type == GV_U32) {
            const uint8_t *v = g->kv[i].value;
            align = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
        }
    if (!align || (align & (align - 1)) || align > 4096)
        r.bad = true;
    uint64_t *offsets = kmalloc(sizeof(uint64_t) * (size_t)(nt + 1));
    if (!offsets) {
        gguf_free(g);
        return E_NOMEM;
    }
    for (uint64_t i = 0; i < nt && !r.bad; i++) {
        struct gguf_tensor *t = &g->t[g->t_count++];
        t->name = rd_str(&r);
        uint32_t dims = rd_u32(&r);
        if (dims < 1 || dims > 4) {
            r.bad = true;
            break;
        }
        t->dims = (int)dims;
        for (uint32_t d = 0; d < 4; d++)
            t->ne[d] = d < dims ? rd_u64(&r) : 1;
        t->type = rd_u32(&r);
        offsets[i] = rd_u64(&r);
    }
    size_t base = ALIGN_UP((size_t)(r.p - buf), (size_t)align);
    for (int i = 0; i < g->t_count && !r.bad; i++) {
        struct gguf_tensor *t = &g->t[i];
        int bytes = 0, per = gguf_block(t->type, &bytes);
        uint64_t n = t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3];
        if (!per) {
            ksnprintf(why, cap, "tensor %.*s has a type this engine does not read (%u)", (int)MIN(t->name.n, 60),
                      t->name.s, t->type);
            kfree(offsets);
            gguf_free(g);
            return E_NOSYS;
        }
        if (t->ne[0] % (uint64_t)per || n > (1ull << 40)) {
            r.bad = true;
            break;
        }
        uint64_t len = n / (uint64_t)per * (uint64_t)bytes;
        if (base > size || offsets[i] > size - base || len > size - base - offsets[i]) {
            r.bad = true;
            break;
        }
        t->data = buf + base + offsets[i];
        t->bytes = (size_t)len;
    }
    kfree(offsets);
    if (r.bad) {
        str_copy(why, "the model file is damaged or cut short", cap);
        gguf_free(g);
        return E_INVAL;
    }
    return 0;
}

static bool gs_eq(struct gguf_str s, const char *z)
{
    size_t n = strlen(z);
    return s.n == n && !memcmp(s.s, z, n);
}

static const struct gguf_kv *gguf_find(const struct gguf *g, const char *key)
{
    for (int i = 0; i < g->kv_count; i++)
        if (gs_eq(g->kv[i].key, key))
            return &g->kv[i];
    return NULL;
}

/* "<arch>.<rest>" */
static const struct gguf_kv *gguf_find_arch(const struct gguf *g, const char *arch, const char *rest)
{
    char key[96];
    ksnprintf(key, sizeof(key), "%s.%s", arch, rest);
    return gguf_find(g, key);
}

static uint64_t gv_uint(const struct gguf_kv *kv, uint64_t def)
{
    if (!kv)
        return def;
    const uint8_t *v = kv->value;
    switch (kv->type) {
    case GV_U8:
    case GV_BOOL: return v[0];
    case GV_I8: return (uint64_t)(int64_t)(int8_t)v[0];
    case GV_U16: return (uint64_t)v[0] | (uint64_t)v[1] << 8;
    case GV_I16: return (uint64_t)(int64_t)(int16_t)(v[0] | v[1] << 8);
    case GV_U32:
    case GV_I32: {
        uint32_t u = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
        return kv->type == GV_I32 ? (uint64_t)(int64_t)(int32_t)u : u;
    }
    case GV_U64:
    case GV_I64: {
        uint64_t u = 0;
        for (int i = 7; i >= 0; i--)
            u = u << 8 | v[i];
        return u;
    }
    }
    return def;
}

static float gv_float(const struct gguf_kv *kv, float def)
{
    if (!kv)
        return def;
    if (kv->type == GV_F32) {
        float f;
        memcpy(&f, kv->value, 4);
        return f;
    }
    if (kv->type == GV_F64) {
        double d;
        memcpy(&d, kv->value, 8);
        return (float)d;
    }
    return (float)(int64_t)gv_uint(kv, 0);
}

static struct gguf_str gv_str(const struct gguf_kv *kv)
{
    if (!kv || kv->type != GV_STR)
        return (struct gguf_str){"", 0};
    struct gguf_reader r = {kv->value, kv->value + 8 + (1u << 30), false};
    return rd_str(&r);
}

/* an array: its item type and count; *items points at the first item */
static bool gv_array(const struct gguf_kv *kv, uint32_t *type, uint64_t *count, const uint8_t **items)
{
    if (!kv || kv->type != GV_ARR)
        return false;
    struct gguf_reader r = {kv->value, kv->value + 12, false};
    *type = rd_u32(&r);
    *count = rd_u64(&r);
    *items = kv->value + 12;
    return true;
}

static const struct gguf_tensor *gguf_tensor(const struct gguf *g, const char *name)
{
    for (int i = 0; i < g->t_count; i++)
        if (gs_eq(g->t[i].name, name))
            return &g->t[i];
    return NULL;
}
