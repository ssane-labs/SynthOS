/* PDF: objects are found by scanning for "N G obj" (and inside object streams), so damaged cross-reference tables do
   not matter. Pages are walked from the catalog; their content streams are run with the text, path and image
   operators that show the content: text where it is (fonts mapped through ToUnicode or WinAnsi), rectangles, lines,
   JPEG and 8-bit images. Numbers are 16.16 fixed point. */

typedef int64_t fx;
#define FX_ONE 65536
#define FX(i) ((fx)(i) * FX_ONE)

static fx fx_mul(fx a, fx b)
{
    return (a * b) / FX_ONE;
}

struct pobj {
    const uint8_t *p, *end;
};

struct cmap_pair {
    uint32_t code, value, more; /* more: a second character (ligatures such as "fi") */
};

struct pfont {
    char name[32];
    int bytes; /* 1 or 2 per code */
    bool bold, mono, italic;
    struct cmap_pair *uni; /* code -> unicode, sorted */
    int uni_count;
    struct cmap_pair *widths; /* code -> width in 1/1000 em, sorted */
    int width_count, dw;
};

#define PDF_FONTS 48
#define PDF_DEPTH 8

struct pdf {
    const uint8_t *data;
    size_t size;
    struct pobj *objs;
    int obj_count;
    uint8_t **bufs; /* decoded object streams */
    int buf_count, buf_cap;
    struct doc *doc;
    struct pfont fonts[PDF_FONTS];
    const uint8_t *font_keys[PDF_FONTS];
    int font_count;
    int32_t page_x0, page_y1;
    int images;
};

static bool pdf_ws(uint8_t c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0;
}

static bool pdf_delim(uint8_t c)
{
    return pdf_ws(c) || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')' || c == '{' ||
           c == '}' || c == '%';
}

static const uint8_t *skip_ws(const uint8_t *p, const uint8_t *end)
{
    if (!p) /* a value that was not there (a reference to a missing object): nothing to read */
        return end;
    while (p < end) {
        if (pdf_ws(*p))
            p++;
        else if (*p == '%')
            while (p < end && *p != '\n' && *p != '\r')
                p++;
        else
            break;
    }
    return p;
}

static const uint8_t *skip_token(const uint8_t *p, const uint8_t *end)
{
    while (p < end && !pdf_delim(*p))
        p++;
    return p;
}

static bool is_int_token(const uint8_t *p, const uint8_t *e)
{
    if (p >= e)
        return false;
    for (; p < e; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

static const uint8_t *skip_value(const uint8_t *p, const uint8_t *end, int depth);

/* after a number: "G R" makes it a reference */
static const uint8_t *maybe_ref(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *a = skip_ws(p, end), *ae = skip_token(a, end);
    if (!is_int_token(a, ae))
        return p;
    const uint8_t *r = skip_ws(ae, end);
    if (r < end && *r == 'R' && (r + 1 >= end || pdf_delim(r[1])))
        return r + 1;
    return p;
}

static const uint8_t *skip_value(const uint8_t *p, const uint8_t *end, int depth)
{
    p = skip_ws(p, end);
    if (p >= end || depth > 32)
        return end;
    if (*p == '/')
        return skip_token(p + 1, end);
    if (*p == '(') {
        int nest = 0;
        for (; p < end; p++) {
            if (*p == '\\')
                p++;
            else if (*p == '(')
                nest++;
            else if (*p == ')' && !--nest)
                return p + 1;
        }
        return end;
    }
    if (*p == '<' && p + 1 < end && p[1] == '<') {
        p += 2;
        for (;;) {
            p = skip_ws(p, end);
            if (p >= end)
                return end;
            if (*p == '>' && p + 1 < end && p[1] == '>')
                return p + 2;
            p = skip_value(p, end, depth + 1);
        }
    }
    if (*p == '<') {
        const uint8_t *e = memchr(p, '>', (size_t)(end - p));
        return e ? e + 1 : end;
    }
    if (*p == '[') {
        p++;
        for (;;) {
            p = skip_ws(p, end);
            if (p >= end)
                return end;
            if (*p == ']')
                return p + 1;
            p = skip_value(p, end, depth + 1);
        }
    }
    if (*p == ']' || *p == '>' || *p == ')' || *p == '{' || *p == '}')
        return p + 1;
    const uint8_t *e = skip_token(p, end);
    if (e == p)
        return p + 1;
    return is_int_token(p, e) ? maybe_ref(e, end) : e;
}

static bool name_is(const uint8_t *v, const uint8_t *end, const char *name)
{
    size_t n = strlen(name);
    return v && v + n <= end && !memcmp(v, name, n) && (v + n == end || pdf_delim(v[n]));
}

static const uint8_t *pdf_deref(struct pdf *pdf, const uint8_t *v, const uint8_t **end);

static long pdf_int(const uint8_t *p, const uint8_t *end)
{
    p = skip_ws(p, end);
    bool neg = p < end && *p == '-';
    if (p < end && (*p == '-' || *p == '+'))
        p++;
    long v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++)
        if (v < 100000000L) /* more digits than any count or offset in a file: it stops growing */
            v = v * 10 + (*p - '0');
    return neg ? -v : v;
}

static fx pdf_num(const uint8_t *p, const uint8_t *end)
{
    p = skip_ws(p, end);
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+'))
        neg = *p++ == '-';
    fx v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++)
        v = MIN(v * 10 + (*p - '0'), (fx)1 << 40);
    v *= FX_ONE;
    if (p < end && *p == '.') {
        fx scale = FX_ONE;
        for (p++; p < end && *p >= '0' && *p <= '9'; p++) {
            scale /= 10;
            v += (*p - '0') * scale;
        }
    }
    return neg ? -v : v;
}

/* the value of /key in the dictionary at p (references followed) */
static const uint8_t *dict_get(struct pdf *pdf, const uint8_t *p, const uint8_t *end, const char *key,
                               const uint8_t **vend)
{
    if (!p)
        return NULL;
    p = skip_ws(p, end);
    if (p + 1 >= end || p[0] != '<' || p[1] != '<')
        return NULL;
    p += 2;
    size_t klen = strlen(key);
    for (int guard = 0; guard < 4096; guard++) {
        p = skip_ws(p, end);
        if (p >= end || *p != '/')
            return NULL;
        const uint8_t *k = p + 1, *ke = skip_token(k, end);
        const uint8_t *v = skip_ws(ke, end);
        if ((size_t)(ke - k) == klen && !memcmp(k, key, klen)) {
            *vend = end;
            return pdf_deref(pdf, v, vend);
        }
        p = skip_value(v, end, 0);
    }
    return NULL;
}

static const uint8_t *pdf_deref(struct pdf *pdf, const uint8_t *v, const uint8_t **end)
{
    for (int i = 0; v && i < 8; i++) {
        v = skip_ws(v, *end);
        const uint8_t *e = skip_token(v, *end);
        if (!is_int_token(v, e) || maybe_ref(e, *end) == e)
            return v;
        long n = pdf_int(v, e);
        if (n < 0 || n >= pdf->obj_count || !pdf->objs[n].p)
            return NULL;
        v = pdf->objs[n].p;
        *end = pdf->objs[n].end;
    }
    return v;
}

static const uint8_t *pdf_obj(struct pdf *pdf, long n, const uint8_t **end)
{
    if (n < 0 || n >= pdf->obj_count || !pdf->objs[n].p)
        return NULL;
    *end = pdf->objs[n].end;
    return skip_ws(pdf->objs[n].p, *end);
}

/* ---- streams ---- */

static const uint8_t *stream_raw(struct pdf *pdf, const uint8_t *dict, const uint8_t *end, size_t *len)
{
    const uint8_t *p = skip_ws(skip_value(dict, end, 0), end);
    if (p + 6 > end || memcmp(p, "stream", 6))
        return NULL;
    p += 6;
    if (p < end && *p == '\r')
        p++;
    if (p < end && *p == '\n')
        p++;
    const uint8_t *le = end, *lv = dict_get(pdf, dict, end, "Length", &le);
    long n = lv ? pdf_int(lv, le) : -1;
    if (n >= 0 && (size_t)n <= (size_t)(pdf->data + pdf->size - p)) {
        *len = (size_t)n;
        return p;
    }
    const uint8_t *e = p;
    for (; e + 9 <= pdf->data + pdf->size && memcmp(e, "endstream", 9); e++)
        ;
    while (e > p && (e[-1] == '\n' || e[-1] == '\r'))
        e--;
    *len = (size_t)(e - p);
    return p;
}

static void unpredict(uint8_t *data, size_t *len, int columns, int colors, int bpc)
{
    int bpp = MAX(1, colors * bpc / 8), row = (columns * colors * bpc + 7) / 8;
    size_t in = 0, out = 0;
    uint8_t *prev = NULL;
    while (in + 1 + (size_t)row <= *len) {
        int type = data[in++];
        uint8_t *cur = data + out;
        memmove(cur, data + in, (size_t)row);
        for (int i = 0; i < row; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            int pv = a + b - c, pa = pv > a ? pv - a : a - pv, pb = pv > b ? pv - b : b - pv, pc = pv > c ? pv - c : c - pv;
            int add = type == 1 ? a : type == 2 ? b : type == 3 ? (a + b) / 2
                      : type == 4 ? (pa <= pb && pa <= pc ? a : pb <= pc ? b : c) : 0;
            cur[i] = (uint8_t)(cur[i] + add);
        }
        prev = cur;
        in += (size_t)row;
        out += (size_t)row;
    }
    *len = out;
}

/* decoded stream data (kmalloc'd); jpeg is set for DCTDecode, whose data comes back as it is */
static uint8_t *stream_decode(struct pdf *pdf, const uint8_t *dict, const uint8_t *end, size_t *len, bool *jpeg)
{
    size_t raw_len;
    const uint8_t *raw = stream_raw(pdf, dict, end, &raw_len);
    if (!raw)
        return NULL;
    if (jpeg)
        *jpeg = false;
    const uint8_t *fe = end, *f = dict_get(pdf, dict, end, "Filter", &fe);
    char filters[4][24];
    int nf = 0;
    if (f && *f == '/') {
        const uint8_t *e = skip_token(f + 1, fe);
        ksnprintf(filters[nf++], sizeof(filters[0]), "%.*s", (int)(e - f - 1), f + 1);
    } else if (f && *f == '[') {
        const uint8_t *p = f + 1;
        while (nf < 4 && (p = skip_ws(p, fe)) < fe && *p == '/') {
            const uint8_t *e = skip_token(p + 1, fe);
            ksnprintf(filters[nf++], sizeof(filters[0]), "%.*s", (int)(e - p - 1), p + 1);
            p = e;
        }
    }
    uint8_t *data = kmalloc(raw_len + 1);
    if (!data)
        return NULL;
    memcpy(data, raw, raw_len);
    size_t n = raw_len;
    for (int i = 0; i < nf; i++) {
        if (!strcmp(filters[i], "FlateDecode") || !strcmp(filters[i], "Fl")) {
            uint8_t *out;
            size_t out_len, used;
            if (zlib_inflate_alloc(data, n, 256u << 20, &out, &out_len) &&
                (n < 2 || inflate_alloc(data + 2, n - 2, 256u << 20, &out, &out_len, &used))) {
                kfree(data);
                return NULL;
            }
            kfree(data);
            data = out;
            n = out_len;
            const uint8_t *pe = end, *parms = dict_get(pdf, dict, end, "DecodeParms", &pe);
            const uint8_t *ve = pe, *pred = parms ? dict_get(pdf, parms, pe, "Predictor", &ve) : NULL;
            if (pred && pdf_int(pred, ve) >= 10) {
                const uint8_t *ce = pe, *cols = dict_get(pdf, parms, pe, "Columns", &ce);
                const uint8_t *ke = pe, *colors = dict_get(pdf, parms, pe, "Colors", &ke);
                const uint8_t *be = pe, *bpc = dict_get(pdf, parms, pe, "BitsPerComponent", &be);
                unpredict(data, &n, cols ? (int)pdf_int(cols, ce) : 1, colors ? (int)pdf_int(colors, ke) : 1,
                          bpc ? (int)pdf_int(bpc, be) : 8);
            }
        } else if (!strcmp(filters[i], "DCTDecode") || !strcmp(filters[i], "DCT")) {
            if (jpeg)
                *jpeg = true;
            break;
        } else {
            kfree(data);
            return NULL;
        }
    }
    *len = n;
    return data;
}

/* ---- finding the objects ---- */

static void add_obj(struct pdf *pdf, long num, const uint8_t *p, const uint8_t *end, bool replace)
{
    if (num < 0 || num >= pdf->obj_count)
        return;
    if (pdf->objs[num].p && !replace)
        return;
    pdf->objs[num] = (struct pobj){p, end};
}

static int pdf_scan(struct pdf *pdf)
{
    const uint8_t *d = pdf->data, *end = d + pdf->size;
    long max = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (const uint8_t *p = d + 1; p + 3 <= end; p++) {
            if (p[0] != 'o' || p[1] != 'b' || p[2] != 'j' || !pdf_ws(p[-1]) || (p + 3 < end && !pdf_delim(p[3])))
                continue;
            const uint8_t *q = p - 1;
            while (q > d && pdf_ws(*q))
                q--;
            const uint8_t *ge = q + 1;
            while (q > d && *q >= '0' && *q <= '9')
                q--;
            if (q + 1 == ge || !pdf_ws(*q))
                continue;
            while (q > d && pdf_ws(*q))
                q--;
            const uint8_t *ne = q + 1;
            while (q > d && *q >= '0' && *q <= '9')
                q--;
            const uint8_t *ns = *q >= '0' && *q <= '9' ? q : q + 1;
            if (ns == ne || (ns > d && !pdf_delim(ns[-1]) && ns[-1] != 0))
                continue;
            long num = pdf_int(ns, ne);
            if (pass == 0) {
                max = MAX(max, num);
                continue;
            }
            const uint8_t *body = p + 3, *e = body;
            for (; e + 6 <= end && memcmp(e, "endobj", 6); e++)
                if (!memcmp(e, "stream", 6)) {
                    size_t len;
                    const uint8_t *s = stream_raw(pdf, body, end, &len);
                    if (s && s + len > e && s + len <= end)
                        e = s + len - 1;
                }
            add_obj(pdf, num, body, e, true);
            p = e > p ? e - 1 : p;
        }
        if (pass == 0) {
            if (max <= 0 || max > 4000000)
                return E_INVAL;
            pdf->obj_count = (int)max + 1;
            pdf->objs = kzalloc((size_t)pdf->obj_count * sizeof(struct pobj));
            if (!pdf->objs)
                return E_NOMEM;
        }
    }
    /* object streams */
    for (int i = 0; i < pdf->obj_count; i++) {
        const uint8_t *e, *o = pdf_obj(pdf, i, &e);
        const uint8_t *te = e, *type = o ? dict_get(pdf, o, e, "Type", &te) : NULL;
        if (!name_is(type, te, "/ObjStm"))
            continue;
        const uint8_t *ne = e, *nv = dict_get(pdf, o, e, "N", &ne);
        const uint8_t *fe2 = e, *fv = dict_get(pdf, o, e, "First", &fe2);
        size_t len;
        uint8_t *data = nv && fv ? stream_decode(pdf, o, e, &len, NULL) : NULL;
        if (!data)
            continue;
        if (!doc_grow((void **)&pdf->bufs, &pdf->buf_cap, pdf->buf_count + 1, sizeof(uint8_t *), 100000)) {
            kfree(data);
            continue;
        }
        pdf->bufs[pdf->buf_count++] = data;
        long n = pdf_int(nv, ne), first = pdf_int(fv, fe2);
        const uint8_t *p = data, *dend = data + len;
        long nums[2], offs[2] = {0, 0};
        for (long k = 0; k < n && (size_t)first < len; k++) {
            const uint8_t *a = skip_ws(p, dend), *ae = skip_token(a, dend);
            const uint8_t *b = skip_ws(ae, dend), *be = skip_token(b, dend);
            if (a >= dend || b >= dend)
                break;
            nums[0] = pdf_int(a, ae);
            offs[0] = pdf_int(b, be);
            p = be;
            const uint8_t *c = skip_ws(p, dend), *ce = skip_token(c, dend);
            const uint8_t *dd = skip_ws(ce, dend), *de = skip_token(dd, dend);
            nums[1] = pdf_int(c, ce);
            offs[1] = k + 1 < n && dd < dend ? pdf_int(dd, de) : (long)len - first;
            if (first + offs[0] < (long)len)
                add_obj(pdf, nums[0], data + first + offs[0], data + MIN((long)len, first + MAX(offs[1], offs[0])),
                        false);
        }
    }
    return 0;
}

/* ---- fonts ---- */

static int pair_cmp_find(const struct cmap_pair *a, int n, uint32_t code)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a[mid].code == code)
            return mid;
        if (a[mid].code < code)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static void pairs_sort(struct cmap_pair *a, int n)
{
    for (int gap = n / 2; gap > 0; gap /= 2) /* shell sort: no recursion, no allocation */
        for (int i = gap; i < n; i++) {
            struct cmap_pair t = a[i];
            int j = i;
            for (; j >= gap && a[j - gap].code > t.code; j -= gap)
                a[j] = a[j - gap];
            a[j] = t;
        }
}

static void pair_add2(struct cmap_pair **a, int *n, int *cap, uint32_t code, uint32_t value, uint32_t more)
{
    if (doc_grow((void **)a, cap, *n + 1, sizeof(struct cmap_pair), 1 << 20))
        (*a)[(*n)++] = (struct cmap_pair){code, value, more};
}

static void pair_add(struct cmap_pair **a, int *n, int *cap, uint32_t code, uint32_t value)
{
    pair_add2(a, n, cap, code, value, 0);
}

static uint32_t hex_value(const uint8_t *p, const uint8_t *e, int *digits)
{
    uint32_t v = 0;
    int n = 0;
    for (; p < e && *p != '>'; p++) {
        int c = *p;
        int h = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
        if (h < 0)
            continue;
        v = v << 4 | (uint32_t)h;
        n++;
    }
    if (digits)
        *digits = n;
    return v;
}

/* UTF-16BE in a hex string: the first character, and the second in *more */
static uint32_t hex_unicode2(const uint8_t *p, const uint8_t *e, uint32_t *more)
{
    uint16_t u[3] = {0, 0, 0};
    int n = 0, digits = 0;
    for (; p < e && *p != '>' && n < 3; p++) {
        int c = *p, h = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
        if (h < 0)
            continue;
        u[n] = (uint16_t)(u[n] << 4 | h);
        if (++digits % 4 == 0)
            n++;
    }
    *more = 0;
    if (u[0] >= 0xd800 && u[0] < 0xdc00 && n >= 2)
        return 0x10000 + ((uint32_t)(u[0] - 0xd800) << 10) + (uint32_t)(u[1] - 0xdc00);
    if (n >= 2)
        *more = u[1];
    return n ? u[0] : 0;
}

static uint32_t hex_unicode(const uint8_t *p, const uint8_t *e)
{
    uint32_t more;
    return hex_unicode2(p, e, &more);
}

static void parse_tounicode(struct pfont *f, const uint8_t *s, size_t len)
{
    const uint8_t *p = s, *end = s + len;
    int cap = 0;
    while (p < end) {
        p = skip_ws(p, end);
        const uint8_t *e = skip_token(p, end);
        if (e - p == 11 && !memcmp(p, "beginbfchar", 11)) {
            p = e;
            for (;;) {
                const uint8_t *a = skip_ws(p, end);
                if (a >= end || *a != '<')
                    break;
                const uint8_t *ae = skip_value(a, end, 0), *b = skip_ws(ae, end), *be = skip_value(b, end, 0);
                uint32_t more;
                if (b < end && *b == '<') {
                    uint32_t u = hex_unicode2(b + 1, be, &more);
                    pair_add2(&f->uni, &f->uni_count, &cap, hex_value(a + 1, ae, NULL), u, more);
                }
                p = be;
            }
        } else if (e - p == 12 && !memcmp(p, "beginbfrange", 12)) {
            p = e;
            for (;;) {
                const uint8_t *a = skip_ws(p, end);
                if (a >= end || *a != '<')
                    break;
                const uint8_t *ae = skip_value(a, end, 0), *b = skip_ws(ae, end), *be = skip_value(b, end, 0);
                const uint8_t *c = skip_ws(be, end), *ce = skip_value(c, end, 0);
                uint32_t lo = hex_value(a + 1, ae, NULL), hi = hex_value(b + 1, be, NULL);
                if (hi - lo < 65536 && c < end) {
                    if (*c == '<') {
                        uint32_t u = hex_unicode(c + 1, ce);
                        for (uint32_t k = lo; k <= hi; k++)
                            pair_add(&f->uni, &f->uni_count, &cap, k, u + (k - lo));
                    } else if (*c == '[') {
                        const uint8_t *q = c + 1;
                        for (uint32_t k = lo; k <= hi; k++) {
                            q = skip_ws(q, ce);
                            if (q >= ce || *q != '<')
                                break;
                            const uint8_t *qe = skip_value(q, ce, 0);
                            uint32_t more, u = hex_unicode2(q + 1, qe, &more);
                            pair_add2(&f->uni, &f->uni_count, &cap, k, u, more);
                            q = qe;
                        }
                    }
                }
                p = ce;
            }
        } else {
            p = e > p ? e : p + 1;
        }
    }
    pairs_sort(f->uni, f->uni_count);
}

static void load_font(struct pdf *pdf, struct pfont *f, const uint8_t *fd, const uint8_t *end)
{
    memset(f, 0, sizeof(*f));
    f->bytes = 1;
    f->dw = 1000;
    const uint8_t *be = end, *base = dict_get(pdf, fd, end, "BaseFont", &be);
    if (base && *base == '/') {
        const uint8_t *e = skip_token(base + 1, be);
        ksnprintf(f->name, sizeof(f->name), "%.*s", (int)MIN(e - base - 1, 31), base + 1);
        f->bold = strstr(f->name, "Bold") || strstr(f->name, "Black") || strstr(f->name, "Heavy");
        f->italic = strstr(f->name, "Italic") || strstr(f->name, "Oblique");
        f->mono = strstr(f->name, "Courier") || strstr(f->name, "Mono");
    }
    const uint8_t *se = end, *sub = dict_get(pdf, fd, end, "Subtype", &se);
    int wcap = 0;
    if (name_is(sub, se, "/Type0")) {
        f->bytes = 2;
        const uint8_t *de = end, *desc = dict_get(pdf, fd, end, "DescendantFonts", &de);
        if (desc && *desc == '[') {
            const uint8_t *ce = de, *cid = pdf_deref(pdf, desc + 1, &ce);
            const uint8_t *we = ce, *w = cid ? dict_get(pdf, cid, ce, "W", &we) : NULL;
            const uint8_t *dwe = ce, *dw = cid ? dict_get(pdf, cid, ce, "DW", &dwe) : NULL;
            if (dw)
                f->dw = (int)pdf_int(dw, dwe);
            if (w && *w == '[') {
                const uint8_t *p = w + 1;
                for (int guard = 0; guard < 65536; guard++) {
                    p = skip_ws(p, we);
                    if (p >= we || *p == ']')
                        break;
                    long c1 = pdf_int(p, we);
                    p = skip_ws(skip_token(p, we), we);
                    if (p < we && *p == '[') {
                        p++;
                        for (long c = c1;; c++) {
                            p = skip_ws(p, we);
                            if (p >= we || *p == ']')
                                break;
                            pair_add(&f->widths, &f->width_count, &wcap, (uint32_t)c,
                                     (uint32_t)(pdf_num(p, we) / FX_ONE));
                            p = skip_token(p, we);
                        }
                        p++;
                    } else {
                        long c2 = pdf_int(p, we);
                        p = skip_ws(skip_token(p, we), we);
                        uint32_t wv = (uint32_t)(pdf_num(p, we) / FX_ONE);
                        p = skip_token(p, we);
                        for (long c = c1; c <= c2 && c - c1 < 65536; c++)
                            pair_add(&f->widths, &f->width_count, &wcap, (uint32_t)c, wv);
                    }
                }
            }
        }
    } else {
        const uint8_t *fce = end, *fc = dict_get(pdf, fd, end, "FirstChar", &fce);
        const uint8_t *we = end, *w = dict_get(pdf, fd, end, "Widths", &we);
        long first = fc ? pdf_int(fc, fce) : 0;
        if (w && *w == '[') {
            const uint8_t *p = w + 1;
            for (long c = first; c < first + 256; c++) {
                p = skip_ws(p, we);
                if (p >= we || *p == ']')
                    break;
                const uint8_t *pe = we, *v = pdf_deref(pdf, p, &pe);
                pair_add(&f->widths, &f->width_count, &wcap, (uint32_t)c, (uint32_t)(pdf_num(v, pe) / FX_ONE));
                p = skip_value(p, we, 0);
            }
        }
        f->dw = f->width_count ? 0 : 500;
    }
    pairs_sort(f->widths, f->width_count);
    const uint8_t *te = end, *tu = dict_get(pdf, fd, end, "ToUnicode", &te);
    if (tu && tu[0] == '<' && tu[1] == '<') {
        size_t len;
        uint8_t *cm = stream_decode(pdf, tu, te, &len, NULL);
        if (cm) {
            parse_tounicode(f, cm, len);
            kfree(cm);
        }
    }
}

static struct pfont *font_for(struct pdf *pdf, const uint8_t *res, const uint8_t *rend, const uint8_t *name,
                              const uint8_t *nend)
{
    const uint8_t *fe = rend, *fonts = dict_get(pdf, res, rend, "Font", &fe);
    char key[64];
    ksnprintf(key, sizeof(key), "%.*s", (int)MIN(nend - name - 1, 63), name + 1);
    const uint8_t *de = fe, *fd = fonts ? dict_get(pdf, fonts, fe, key, &de) : NULL;
    if (!fd)
        return NULL;
    for (int i = 0; i < pdf->font_count; i++)
        if (pdf->font_keys[i] == fd)
            return &pdf->fonts[i];
    int i = pdf->font_count < PDF_FONTS ? pdf->font_count++ : PDF_FONTS - 1;
    if (i == PDF_FONTS - 1 && pdf->font_keys[i]) {
        kfree(pdf->fonts[i].uni);
        kfree(pdf->fonts[i].widths);
    }
    pdf->font_keys[i] = fd;
    load_font(pdf, &pdf->fonts[i], fd, de);
    return &pdf->fonts[i];
}

static const uint16_t win_ansi_high[32] = {
    0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017d, 0,
    0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178};

static uint32_t font_unicode(const struct pfont *f, uint32_t code, uint32_t *more)
{
    *more = 0;
    int i = f ? pair_cmp_find(f->uni, f->uni_count, code) : -1;
    if (i >= 0) {
        *more = f->uni[i].more;
        return f->uni[i].value;
    }
    if (f && f->bytes == 2)
        return f->uni_count ? 0 : '?';
    if (code >= 128 && code < 160)
        return win_ansi_high[code - 128] ? win_ansi_high[code - 128] : '?';
    return code < 32 ? 0 : code;
}

static int font_width(const struct pfont *f, uint32_t code)
{
    int i = f ? pair_cmp_find(f->widths, f->width_count, code) : -1;
    return i >= 0 ? (int)f->widths[i].value : f && f->dw ? f->dw : 500;
}

/* ---- content streams ---- */

struct gstate {
    fx m[6];
    uint32_t fill, stroke;
};

struct tstate {
    fx tm[6], tlm[6];
    fx size, leading, char_space, word_space, scale, rise;
    struct pfont *font;
};

#define MAX_OPS 48
#define MAX_PATH 256

struct runner {
    struct pdf *pdf;
    const uint8_t *res, *rend;
    struct gstate g[16];
    int gs;
    struct tstate t;
    const uint8_t *ops[MAX_OPS], *ops_end;
    int nops;
    struct {
        fx x, y;
        bool move;
    } path[MAX_PATH];
    int npath;
    struct {
        fx x, y, w, h;
    } rects[64];
    int nrects;
    int depth;
};

static void mat_mul(const fx *a, const fx *b, fx *out)
{
    fx r[6] = {fx_mul(a[0], b[0]) + fx_mul(a[1], b[2]), fx_mul(a[0], b[1]) + fx_mul(a[1], b[3]),
               fx_mul(a[2], b[0]) + fx_mul(a[3], b[2]), fx_mul(a[2], b[1]) + fx_mul(a[3], b[3]),
               fx_mul(a[4], b[0]) + fx_mul(a[5], b[2]) + b[4], fx_mul(a[4], b[1]) + fx_mul(a[5], b[3]) + b[5]};
    memcpy(out, r, sizeof(r));
}

static void mat_point(const fx *m, fx x, fx y, fx *ox, fx *oy)
{
    *ox = fx_mul(x, m[0]) + fx_mul(y, m[2]) + m[4];
    *oy = fx_mul(x, m[1]) + fx_mul(y, m[3]) + m[5];
}

static fx fx_hypot(fx a, fx b)
{
    uint64_t v = (uint64_t)(a < 0 ? -a : a) * (uint64_t)(a < 0 ? -a : a) +
                 (uint64_t)(b < 0 ? -b : b) * (uint64_t)(b < 0 ? -b : b);
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (fx)r;
}

static int32_t to_x(struct runner *r, fx x)
{
    return (int32_t)(x * PT / FX_ONE) - r->pdf->page_x0;
}

static int32_t to_y(struct runner *r, fx y)
{
    return r->pdf->page_y1 - (int32_t)(y * PT / FX_ONE);
}

static fx op_num(struct runner *r, int i)
{
    return i < r->nops ? pdf_num(r->ops[i], r->ops_end) : 0;
}

static uint32_t rgb(fx r, fx g, fx b)
{
    int R = (int)CLAMP(r * 255 / FX_ONE, 0, 255), G = (int)CLAMP(g * 255 / FX_ONE, 0, 255),
        B = (int)CLAMP(b * 255 / FX_ONE, 0, 255);
    return 0xff000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
}

static uint32_t op_color(struct runner *r)
{
    int n = 0;
    while (n < r->nops && r->ops[n][0] != '/')
        n++;
    if (n == 1)
        return rgb(op_num(r, 0), op_num(r, 0), op_num(r, 0));
    if (n == 3)
        return rgb(op_num(r, 0), op_num(r, 1), op_num(r, 2));
    if (n == 4) {
        fx k = FX_ONE - op_num(r, 3);
        return rgb(fx_mul(FX_ONE - op_num(r, 0), k), fx_mul(FX_ONE - op_num(r, 1), k), fx_mul(FX_ONE - op_num(r, 2), k));
    }
    return 0xff000000;
}

/* one string of text shown with the current font; the text matrix moves past it */
static void show_string(struct runner *r, const uint8_t *s, const uint8_t *e, char *buf, int *blen, int cap, fx *adv)
{
    uint8_t bytes[1024];
    int n = 0;
    if (*s == '(') {
        int nest = 0;
        for (const uint8_t *p = s; p < e && n < (int)sizeof(bytes); p++) {
            uint8_t c = *p;
            if (c == '\\' && p + 1 < e) {
                c = *++p;
                if (c >= '0' && c <= '7') {
                    int v = 0;
                    for (int k = 0; k < 3 && p < e && *p >= '0' && *p <= '7'; k++, p++)
                        v = v * 8 + (*p - '0');
                    p--;
                    c = (uint8_t)v;
                } else if (c == 'n') {
                    c = '\n';
                } else if (c == 'r') {
                    c = '\r';
                } else if (c == 't') {
                    c = '\t';
                } else if (c == 'b') {
                    c = '\b';
                } else if (c == 'f') {
                    c = '\f';
                } else if (c == '\r' || c == '\n') {
                    continue;
                }
                bytes[n++] = c;
                continue;
            }
            if (c == '(' && !nest++)
                continue;
            if (c == ')' && !--nest)
                break;
            bytes[n++] = c;
        }
    } else {
        int digits = 0;
        uint8_t v = 0;
        for (const uint8_t *p = s + 1; p < e && *p != '>' && n < (int)sizeof(bytes); p++) {
            int c = *p, h = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (h < 0)
                continue;
            v = (uint8_t)(v << 4 | h);
            if (++digits % 2 == 0)
                bytes[n++] = v;
        }
        if (digits % 2)
            bytes[n++] = (uint8_t)(v << 4);
    }
    struct pfont *f = r->t.font;
    int step = f && f->bytes == 2 ? 2 : 1;
    for (int i = 0; i + step <= n; i += step) {
        uint32_t code = step == 2 ? (uint32_t)bytes[i] << 8 | bytes[i + 1] : bytes[i];
        uint32_t more, u = font_unicode(f, code, &more);
        if (u && u != '\r' && u != '\n' && *blen + 8 < cap) {
            *blen += utf8_put(buf + *blen, u == '\t' || u == 0xa0 ? ' ' : u);
            if (more)
                *blen += utf8_put(buf + *blen, more);
        }
        fx w = fx_mul(FX(font_width(f, code)) / 1000, r->t.size) + r->t.char_space;
        if (code == 32 && step == 1)
            w += r->t.word_space;
        *adv += fx_mul(w, r->t.scale);
    }
}

static void emit_text(struct runner *r, const char *buf, int len, const fx *start_tm, fx adv)
{
    if (!len)
        return;
    fx trm[6];
    mat_mul(start_tm, r->g[r->gs].m, trm);
    fx size = fx_mul(r->t.size, fx_hypot(trm[2], trm[3]));
    fx x, y;
    mat_point(trm, 0, r->t.rise, &x, &y);
    struct pfont *f = r->t.font;
    int style = f && f->mono ? ST_MONO : f && f->bold ? ST_BOLD : f && f->italic ? ST_ITALIC : ST_NORMAL;
    fx width = fx_mul(adv, fx_hypot(trm[0], trm[1]));
    fx sz = size * PT / FX_ONE;
    if (sz < PT / 2 || sz > 1000 * PT)
        return;
    doc_text(r->pdf->doc, to_x(r, x), to_y(r, y), (int)sz, style, r->g[r->gs].fill, buf, (size_t)len,
             (int32_t)(width * PT / FX_ONE));
}

static void text_move(struct runner *r, fx tx, fx ty)
{
    fx m[6] = {FX_ONE, 0, 0, FX_ONE, tx, ty};
    mat_mul(m, r->t.tlm, r->t.tlm);
    memcpy(r->t.tm, r->t.tlm, sizeof(r->t.tm));
}

static void show(struct runner *r, const uint8_t *v, const uint8_t *e)
{
    char buf[2048];
    int len = 0;
    fx start[6], adv = 0;
    memcpy(start, r->t.tm, sizeof(start));
    if (*v == '[') {
        const uint8_t *p = v + 1;
        for (;;) {
            p = skip_ws(p, e);
            if (p >= e || *p == ']')
                break;
            const uint8_t *q = skip_value(p, e, 0);
            if (*p == '(' || *p == '<') {
                show_string(r, p, q, buf, &len, (int)sizeof(buf), &adv);
            } else {
                fx k = pdf_num(p, q);
                fx move = fx_mul(-k / 1000, fx_mul(r->t.size, r->t.scale));
                if (k < -FX(180) && len && buf[len - 1] != ' ' && len + 1 < (int)sizeof(buf)) {
                    buf[len++] = ' ';
                } else if (k > FX(800) || k < -FX(2500)) { /* a jump: a new piece of text */
                    emit_text(r, buf, len, start, adv);
                    len = 0;
                    fx m[6] = {FX_ONE, 0, 0, FX_ONE, adv + move, 0};
                    mat_mul(m, start, start);
                    adv = 0;
                    p = q;
                    continue;
                }
                adv += move;
            }
            p = q;
        }
    } else {
        show_string(r, v, e, buf, &len, (int)sizeof(buf), &adv);
    }
    emit_text(r, buf, len, start, adv);
    fx m[6] = {FX_ONE, 0, 0, FX_ONE, 0, 0};
    memcpy(r->t.tm, start, sizeof(start));
    m[4] = adv;
    mat_mul(m, r->t.tm, r->t.tm);
}

static void image_place(struct runner *r, struct picture *pic)
{
    struct doc *d = r->pdf->doc;
    const fx *m = r->g[r->gs].m;
    fx xs[4], ys[4];
    mat_point(m, 0, 0, &xs[0], &ys[0]);
    mat_point(m, FX_ONE, 0, &xs[1], &ys[1]);
    mat_point(m, 0, FX_ONE, &xs[2], &ys[2]);
    mat_point(m, FX_ONE, FX_ONE, &xs[3], &ys[3]);
    fx x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
        x0 = MIN(x0, xs[i]);
        x1 = MAX(x1, xs[i]);
        y0 = MIN(y0, ys[i]);
        y1 = MAX(y1, ys[i]);
    }
    if (!doc_grow((void **)&d->pics, &d->pic_cap, d->pic_count + 1, sizeof(struct picture), 4096)) {
        picture_free(pic);
        return;
    }
    struct item *it = doc_add(d, IT_IMAGE);
    if (!it) {
        picture_free(pic);
        return;
    }
    d->pics[d->pic_count] = *pic;
    *it = (struct item){.kind = IT_IMAGE, .x = to_x(r, x0), .y = to_y(r, y1),
                        .w = (int32_t)((x1 - x0) * PT / FX_ONE), .h = (int32_t)((y1 - y0) * PT / FX_ONE),
                        .text = (uint32_t)d->pic_count++};
}

static void image_xobject(struct runner *r, const uint8_t *o, const uint8_t *e)
{
    if (r->pdf->images >= 400)
        return;
    size_t len;
    bool jpeg;
    uint8_t *data = stream_decode(r->pdf, o, e, &len, &jpeg);
    if (!data)
        return;
    struct picture pic = {0};
    if (jpeg) {
        if (jpeg_decode(data, len, &pic)) {
            kfree(data);
            return;
        }
    } else {
        const uint8_t *we = e, *wv = dict_get(r->pdf, o, e, "Width", &we);
        const uint8_t *he = e, *hv = dict_get(r->pdf, o, e, "Height", &he);
        const uint8_t *be = e, *bv = dict_get(r->pdf, o, e, "BitsPerComponent", &be);
        const uint8_t *ce = e, *cs = dict_get(r->pdf, o, e, "ColorSpace", &ce);
        long w = wv ? pdf_int(wv, we) : 0, h = hv ? pdf_int(hv, he) : 0, bpc = bv ? pdf_int(bv, be) : 8;
        int comps = name_is(cs, ce, "/DeviceRGB") || name_is(cs, ce, "/CalRGB") ? 3
                    : name_is(cs, ce, "/DeviceGray") || name_is(cs, ce, "/CalGray") ? 1 : 0;
        if (cs && *cs == '[') {
            const uint8_t *q = skip_ws(cs + 1, ce);
            if (name_is(q, ce, "/ICCBased")) {
                const uint8_t *se = ce, *icc = pdf_deref(r->pdf, skip_value(q, ce, 0), &se);
                const uint8_t *ne = se, *nv = icc ? dict_get(r->pdf, icc, se, "N", &ne) : NULL;
                comps = nv ? (int)pdf_int(nv, ne) : 3;
                if (comps != 1 && comps != 3)
                    comps = 0;
            }
        }
        if (!comps || bpc != 8 || w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 16u << 20 ||
            (size_t)(w * h * comps) > len) {
            kfree(data);
            return;
        }
        pic.px = kmalloc((size_t)(w * h) * 4);
        if (!pic.px) {
            kfree(data);
            return;
        }
        pic.w = (int)w;
        pic.h = (int)h;
        pic.frames = 1;
        for (long i = 0; i < w * h; i++) {
            const uint8_t *s = data + i * comps;
            pic.px[i] = 0xff000000u | (comps == 3 ? (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]
                                                  : (uint32_t)s[0] * 0x010101u);
        }
    }
    kfree(data);
    r->pdf->images++;
    image_place(r, &pic);
}

static void run_content(struct runner *r, const uint8_t *s, size_t len);

static void do_xobject(struct runner *r, const uint8_t *name, const uint8_t *nend)
{
    if (r->depth >= PDF_DEPTH)
        return;
    const uint8_t *xe = r->rend, *xo = dict_get(r->pdf, r->res, r->rend, "XObject", &xe);
    char key[64];
    ksnprintf(key, sizeof(key), "%.*s", (int)MIN(nend - name - 1, 63), name + 1);
    const uint8_t *oe = xe, *o = xo ? dict_get(r->pdf, xo, xe, key, &oe) : NULL;
    const uint8_t *se = oe, *sub = o ? dict_get(r->pdf, o, oe, "Subtype", &se) : NULL;
    if (name_is(sub, se, "/Image")) {
        image_xobject(r, o, oe);
    } else if (name_is(sub, se, "/Form")) {
        size_t len;
        uint8_t *data = stream_decode(r->pdf, o, oe, &len, NULL);
        if (!data)
            return;
        struct runner *sub_r = kmalloc(sizeof(*sub_r));
        if (sub_r) {
            *sub_r = *r;
            sub_r->depth = r->depth + 1;
            sub_r->gs = 0;
            sub_r->g[0] = r->g[r->gs];
            const uint8_t *me = oe, *m = dict_get(r->pdf, o, oe, "Matrix", &me);
            if (m && *m == '[') {
                fx fm[6];
                const uint8_t *p = m + 1;
                for (int i = 0; i < 6; i++) {
                    fm[i] = pdf_num(p, me);
                    p = skip_value(p, me, 0);
                }
                mat_mul(fm, sub_r->g[0].m, sub_r->g[0].m);
            }
            const uint8_t *re = oe, *res = dict_get(r->pdf, o, oe, "Resources", &re);
            if (res) {
                sub_r->res = res;
                sub_r->rend = re;
            }
            run_content(sub_r, data, len);
            kfree(sub_r);
        }
        kfree(data);
    }
}

static void path_flush(struct runner *r, bool fill, bool stroke)
{
    struct doc *d = r->pdf->doc;
    for (int i = 0; i < r->nrects; i++) {
        fx x0, y0, x1, y1;
        mat_point(r->g[r->gs].m, r->rects[i].x, r->rects[i].y, &x0, &y0);
        mat_point(r->g[r->gs].m, r->rects[i].x + r->rects[i].w, r->rects[i].y + r->rects[i].h, &x1, &y1);
        int32_t ax = to_x(r, MIN(x0, x1)), ay = to_y(r, MAX(y0, y1)), w = (int32_t)((x1 > x0 ? x1 - x0 : x0 - x1) * PT / FX_ONE),
                h = (int32_t)((y1 > y0 ? y1 - y0 : y0 - y1) * PT / FX_ONE);
        if (fill && r->g[r->gs].fill != 0xffffffff)
            doc_shape(d, IT_RECT, ax, ay, MAX(w, PT / 2), MAX(h, PT / 2), r->g[r->gs].fill);
        else if (stroke)
            doc_shape(d, IT_FRAME, ax, ay, w, h, r->g[r->gs].stroke);
    }
    if (stroke)
        for (int i = 1; i < r->npath; i++) {
            if (r->path[i].move)
                continue;
            fx x0, y0, x1, y1;
            mat_point(r->g[r->gs].m, r->path[i - 1].x, r->path[i - 1].y, &x0, &y0);
            mat_point(r->g[r->gs].m, r->path[i].x, r->path[i].y, &x1, &y1);
            doc_shape(d, IT_LINE, to_x(r, x0), to_y(r, y0), (int32_t)((x1 - x0) * PT / FX_ONE),
                      -(int32_t)((y1 - y0) * PT / FX_ONE), r->g[r->gs].stroke);
        }
    r->npath = r->nrects = 0;
}

static void path_point(struct runner *r, fx x, fx y, bool move)
{
    if (r->npath < MAX_PATH)
        r->path[r->npath++] = (typeof(r->path[0])){x, y, move};
}

static bool op_is(const uint8_t *p, const uint8_t *e, const char *op)
{
    size_t n = strlen(op);
    return (size_t)(e - p) == n && !memcmp(p, op, n);
}

static void run_op(struct runner *r, const uint8_t *op, const uint8_t *oe)
{
    struct gstate *g = &r->g[r->gs];
    struct tstate *t = &r->t;
    int n = r->nops;
    switch (oe - op) {
    case 1:
        switch (*op) {
        case 'q':
            if (r->gs < 15) {
                r->g[r->gs + 1] = *g;
                r->gs++;
            }
            return;
        case 'Q':
            if (r->gs > 0)
                r->gs--;
            return;
        case 'g': g->fill = op_color(r); return;
        case 'G': g->stroke = op_color(r); return;
        case 'k': g->fill = op_color(r); return;
        case 'K': g->stroke = op_color(r); return;
        case 'm': path_point(r, op_num(r, 0), op_num(r, 1), true); return;
        case 'l': path_point(r, op_num(r, 0), op_num(r, 1), false); return;
        case 'c': path_point(r, op_num(r, 4), op_num(r, 5), false); return;
        case 'v':
        case 'y': path_point(r, op_num(r, 2), op_num(r, 3), false); return;
        case 'h':
            for (int i = r->npath - 1; i >= 0; i--)
                if (r->path[i].move) {
                    path_point(r, r->path[i].x, r->path[i].y, false);
                    break;
                }
            return;
        case 'f':
        case 'F': path_flush(r, true, false); return;
        case 'B':
        case 'b': path_flush(r, true, true); return;
        case 'S':
        case 's': path_flush(r, false, true); return;
        case 'n':
            r->npath = r->nrects = 0;
            return;
        case '\'':
            text_move(r, 0, -t->leading);
            if (n >= 1)
                show(r, r->ops[0], r->ops_end);
            return;
        case '"':
            t->word_space = op_num(r, 0);
            t->char_space = op_num(r, 1);
            text_move(r, 0, -t->leading);
            if (n >= 3)
                show(r, r->ops[2], r->ops_end);
            return;
        }
        return;
    case 2:
        if (op_is(op, oe, "cm") && n >= 6) {
            fx m[6];
            for (int i = 0; i < 6; i++)
                m[i] = op_num(r, i);
            mat_mul(m, g->m, g->m);
        } else if (op_is(op, oe, "rg") || op_is(op, oe, "sc")) {
            g->fill = op_color(r);
        } else if (op_is(op, oe, "RG") || op_is(op, oe, "SC")) {
            g->stroke = op_color(r);
        } else if (op_is(op, oe, "re") && n >= 4 && r->nrects < 64) {
            r->rects[r->nrects].x = op_num(r, 0);
            r->rects[r->nrects].y = op_num(r, 1);
            r->rects[r->nrects].w = op_num(r, 2);
            r->rects[r->nrects++].h = op_num(r, 3);
        } else if (op_is(op, oe, "f*") || op_is(op, oe, "B*") || op_is(op, oe, "b*")) {
            path_flush(r, true, *op != 'f');
        } else if (op_is(op, oe, "BT")) {
            fx id[6] = {FX_ONE, 0, 0, FX_ONE, 0, 0};
            memcpy(t->tm, id, sizeof(id));
            memcpy(t->tlm, id, sizeof(id));
        } else if (op_is(op, oe, "Tf") && n >= 2) {
            if (r->ops[0][0] == '/')
                t->font = font_for(r->pdf, r->res, r->rend, r->ops[0], skip_token(r->ops[0] + 1, r->ops_end));
            t->size = op_num(r, 1);
        } else if (op_is(op, oe, "Td") && n >= 2) {
            text_move(r, op_num(r, 0), op_num(r, 1));
        } else if (op_is(op, oe, "TD") && n >= 2) {
            t->leading = -op_num(r, 1);
            text_move(r, op_num(r, 0), op_num(r, 1));
        } else if (op_is(op, oe, "Tm") && n >= 6) {
            for (int i = 0; i < 6; i++)
                t->tlm[i] = op_num(r, i);
            memcpy(t->tm, t->tlm, sizeof(t->tm));
        } else if (op_is(op, oe, "T*")) {
            text_move(r, 0, -t->leading);
        } else if (op_is(op, oe, "TL")) {
            t->leading = op_num(r, 0);
        } else if (op_is(op, oe, "Tc")) {
            t->char_space = op_num(r, 0);
        } else if (op_is(op, oe, "Tw")) {
            t->word_space = op_num(r, 0);
        } else if (op_is(op, oe, "Tz")) {
            t->scale = op_num(r, 0) / 100;
        } else if (op_is(op, oe, "Ts")) {
            t->rise = op_num(r, 0);
        } else if ((op_is(op, oe, "Tj") || op_is(op, oe, "TJ")) && n >= 1) {
            show(r, r->ops[n - 1], r->ops_end);
        } else if (op_is(op, oe, "Do") && n >= 1 && r->ops[0][0] == '/') {
            do_xobject(r, r->ops[0], skip_token(r->ops[0] + 1, r->ops_end));
        }
        return;
    case 3:
        if (op_is(op, oe, "scn"))
            g->fill = op_color(r);
        else if (op_is(op, oe, "SCN"))
            g->stroke = op_color(r);
        return;
    }
}

static void run_content(struct runner *r, const uint8_t *s, size_t len)
{
    const uint8_t *p = s, *end = s + len;
    r->ops_end = end;
    r->nops = 0;
    while ((p = skip_ws(p, end)) < end) {
        uint8_t c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '"') {
            const uint8_t *e = c == '\'' || c == '"' ? p + 1 : skip_token(p, end);
            if (op_is(p, e, "BI")) { /* inline image: skipped */
                const uint8_t *q = e;
                for (; q + 2 < end && !(pdf_ws(q[-1]) && q[0] == 'E' && q[1] == 'I' && pdf_delim(q[2])); q++)
                    ;
                p = q + 2;
                r->nops = 0;
                continue;
            }
            if (!op_is(p, e, "true") && !op_is(p, e, "false") && !op_is(p, e, "null")) {
                run_op(r, p, e);
                r->nops = 0;
                p = e;
                continue;
            }
        }
        const uint8_t *e = skip_value(p, end, 0);
        if (e <= p)
            e = p + 1;
        if (r->nops < MAX_OPS)
            r->ops[r->nops++] = p;
        else
            r->nops = 0;
        p = e;
    }
}

/* ---- pages ---- */

static void pdf_page(struct pdf *pdf, const uint8_t *page, const uint8_t *end, const uint8_t *box, const uint8_t *bend,
                     const uint8_t *res, const uint8_t *rend)
{
    fx b[4] = {0, 0, FX(612), FX(792)};
    if (box && *box == '[') {
        const uint8_t *p = box + 1;
        for (int i = 0; i < 4; i++) {
            const uint8_t *ve = bend, *v = pdf_deref(pdf, p, &ve);
            b[i] = pdf_num(v, ve);
            p = skip_value(p, bend, 0);
        }
    }
    fx w = b[2] - b[0], h = b[3] - b[1];
    if (w <= 0 || h <= 0 || w > FX(14400) || h > FX(14400)) {
        w = FX(612);
        h = FX(792);
    }
    doc_new_page(pdf->doc, (int32_t)(w * PT / FX_ONE), (int32_t)(h * PT / FX_ONE));
    pdf->page_x0 = (int32_t)(MIN(b[0], b[2]) * PT / FX_ONE);
    pdf->page_y1 = (int32_t)(MAX(b[1], b[3]) * PT / FX_ONE);
    struct runner *r = kzalloc(sizeof(*r));
    if (!r)
        return;
    r->pdf = pdf;
    r->res = res;
    r->rend = rend;
    fx id[6] = {FX_ONE, 0, 0, FX_ONE, 0, 0};
    memcpy(r->g[0].m, id, sizeof(id));
    r->g[0].fill = r->g[0].stroke = 0xff000000;
    r->t.scale = FX_ONE;
    const uint8_t *ce = end, *c = dict_get(pdf, page, end, "Contents", &ce);
    if (c && *c == '[') {
        const uint8_t *p = c + 1;
        uint8_t *all = NULL;
        size_t total = 0;
        for (int guard = 0; guard < 4096; guard++) {
            p = skip_ws(p, ce);
            if (p >= ce || *p == ']')
                break;
            const uint8_t *se = ce, *s = pdf_deref(pdf, p, &se);
            size_t len;
            uint8_t *data = s ? stream_decode(pdf, s, se, &len, NULL) : NULL;
            if (data) {
                uint8_t *grown = krealloc(all, total + len + 1);
                if (grown) {
                    all = grown;
                    memcpy(all + total, data, len);
                    total += len;
                    all[total++] = '\n';
                }
                kfree(data);
            }
            p = skip_value(p, ce, 0);
        }
        if (all)
            run_content(r, all, total);
        kfree(all);
    } else if (c) {
        size_t len;
        uint8_t *data = stream_decode(pdf, c, ce, &len, NULL);
        if (data) {
            run_content(r, data, len);
            kfree(data);
        }
    }
    kfree(r);
}

static long ref_num(const uint8_t *v, const uint8_t *end)
{
    v = skip_ws(v, end);
    const uint8_t *e = skip_token(v, end);
    return is_int_token(v, e) && maybe_ref(e, end) != e ? pdf_int(v, e) : -1;
}

static void pdf_pages(struct pdf *pdf, const uint8_t *node, const uint8_t *end, const uint8_t *box,
                      const uint8_t *bend, const uint8_t *res, const uint8_t *rend, int depth, long num)
{
    if (!node || depth > 32 || pdf->doc->page_count >= MAX_PAGES)
        return;
    const uint8_t *e2 = end, *v = dict_get(pdf, node, end, "MediaBox", &e2);
    if (v) {
        box = v;
        bend = e2;
    }
    const uint8_t *e3 = end, *rv = dict_get(pdf, node, end, "Resources", &e3);
    if (rv) {
        res = rv;
        rend = e3;
    }
    const uint8_t *ke = end, *kids = dict_get(pdf, node, end, "Kids", &ke);
    if (kids && *kids == '[') {
        const uint8_t *p = kids + 1;
        for (int guard = 0; guard < 100000; guard++) {
            p = skip_ws(p, ke);
            if (p >= ke || *p == ']')
                break;
            const uint8_t *ce = ke, *child = pdf_deref(pdf, p, &ce);
            if (child && child != node)
                pdf_pages(pdf, child, ce, box, bend, res, rend, depth + 1, ref_num(p, ke));
            p = skip_value(p, ke, 0);
        }
        return;
    }
    struct doc *d = pdf->doc;
    int before = d->page_count;
    pdf_page(pdf, node, end, box, bend, res, rend);
    int *po = d->page_count > before ? krealloc(d->page_obj, (size_t)d->page_count * sizeof(int)) : NULL;
    if (po) {
        d->page_obj = po;
        po[d->page_count - 1] = (int)num;
    }
}

static const uint8_t *memmem_pages(const uint8_t *p, const uint8_t *end)
{
    for (; p + 6 < end; p++)
        if (!memcmp(p, "/Pages", 6) && pdf_delim(p[6]))
            return p + 6;
    return NULL;
}

static bool pdf_encrypted(const uint8_t *data, size_t size)
{
    size_t from = size > 65536 ? size - 65536 : 0;
    for (size_t i = from; i + 8 <= size; i++)
        if (data[i] == '/' && !memcmp(data + i, "/Encrypt", 8) && pdf_delim(data[i + 8 < size ? i + 8 : i]))
            return true;
    return false;
}

static int pdf_read(struct doc *d, const uint8_t *data, size_t size)
{
    if (size < 8 || memcmp(data, "%PDF", 4))
        return E_INVAL;
    if (pdf_encrypted(data, size))
        return E_PERM;
    struct pdf *pdf = kzalloc(sizeof(*pdf));
    if (!pdf)
        return E_NOMEM;
    pdf->data = data;
    pdf->size = size;
    pdf->doc = d;
    d->kind = "PDF document";
    int err = pdf_scan(pdf);
    if (!err) {
        const uint8_t *root = NULL, *rend = NULL;
        /* the catalog: the newest object of /Type /Catalog */
        for (int i = pdf->obj_count - 1; i >= 0 && !root; i--) {
            const uint8_t *e, *o = pdf_obj(pdf, i, &e);
            const uint8_t *te = e, *t = o ? dict_get(pdf, o, e, "Type", &te) : NULL;
            if (name_is(t, te, "/Catalog")) {
                root = o;
                rend = e;
                d->root_obj = i;
            }
        }
        d->obj_count = pdf->obj_count;
        const uint8_t *pe = rend, *pages = root ? dict_get(pdf, root, rend, "Pages", &pe) : NULL;
        if (pages) {
            /* the reference itself, for saving changes */
            const uint8_t *k = root ? memmem_pages(root, rend) : NULL;
            d->pages_obj = k ? (int)ref_num(k, rend) : -1;
            pdf_pages(pdf, pages, pe, NULL, NULL, NULL, NULL, 0, d->pages_obj);
        }
        if (!d->page_count)
            err = E_INVAL;
    }
    for (int i = 0; i < pdf->font_count; i++) {
        kfree(pdf->fonts[i].uni);
        kfree(pdf->fonts[i].widths);
    }
    for (int i = 0; i < pdf->buf_count; i++)
        kfree(pdf->bufs[i]);
    kfree(pdf->bufs);
    kfree(pdf->objs);
    kfree(pdf);
    return err;
}

/* Deletes a page: an incremental update appends a new page tree root listing the other pages, so the file keeps
   everything else as it was (and Rewind keeps the version before). */
static int pdf_delete_page(const char *path, const struct doc *d, int page)
{
    if (!d->page_obj || d->pages_obj <= 0 || d->root_obj <= 0 || d->page_count < 2 || page < 0 ||
        page >= d->page_count)
        return E_INVAL;
    for (int i = 0; i < d->page_count; i++)
        if (d->page_obj[i] <= 0)
            return E_INVAL;
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    long prev = -1;
    for (size_t i = size > 9 ? size - 9 : 0; i > 0 && i + 1024 > size; i--)
        if (!memcmp(data + i, "startxref", 9)) {
            prev = pdf_int((const uint8_t *)data + i + 9, (const uint8_t *)data + size);
            break;
        }
    size_t cap = size + 256 + (size_t)d->page_count * 16;
    char *out = krealloc(data, cap);
    if (!out) {
        kfree(data);
        return E_NOMEM;
    }
    size_t n = size;
    if (out[n - 1] != '\n')
        out[n++] = '\n';
    size_t at = n;
    n += (size_t)ksnprintf(out + n, cap - n, "%d 0 obj\n<< /Type /Pages /Count %d /Kids [", d->pages_obj, d->page_count - 1);
    for (int i = 0; i < d->page_count; i++)
        if (i != page)
            n += (size_t)ksnprintf(out + n, cap - n, "%d 0 R ", d->page_obj[i]);
    n += (size_t)ksnprintf(out + n, cap - n, "] >>\nendobj\n");
    size_t xref = n;
    n += (size_t)ksnprintf(out + n, cap - n, "xref\n0 1\n0000000000 65535 f \n%d 1\n%010u 00000 n \ntrailer\n<< /Size %d /Root %d 0 R",
                           d->pages_obj, (unsigned)at, MAX(d->obj_count, d->pages_obj + 1), d->root_obj);
    if (prev >= 0)
        n += (size_t)ksnprintf(out + n, cap - n, " /Prev %ld", prev);
    n += (size_t)ksnprintf(out + n, cap - n, " >>\nstartxref\n%u\n%%%%EOF\n", (unsigned)xref);
    err = vfs_write_all(path, out, n);
    kfree(out);
    return err;
}
