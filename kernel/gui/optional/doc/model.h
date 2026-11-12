/* The document model of the Documents app: pages of positioned items in 1/16 pt, y down. PDF pages are read into it
   as they are; text documents (DOCX, ODT, plain text) flow into A4 pages; spreadsheets become tables; slides keep the
   places of their text boxes. Included by documents.c (and the host test), so everything is static. */

#define PT 16
#define A4_W (595 * PT)
#define A4_H (842 * PT)
#define MARGIN (64 * PT)
#define MAX_ITEMS 200000
#define MAX_PAGES 4000
#define DOC_TEXT_MAX (16u << 20)

enum item_kind { IT_TEXT, IT_RECT, IT_FRAME, IT_LINE, IT_IMAGE };
enum text_style { ST_NORMAL, ST_BOLD, ST_MONO, ST_ITALIC };

struct item {
    uint8_t kind, style;
    uint16_t size;    /* text: 1/16 pt */
    int32_t x, y, w, h; /* text: x, baseline y, run width; line: from (x, y) by (w, h) */
    uint32_t color;
    uint32_t text, len; /* text: UTF-8 in doc->text; image: index in doc->pics */
};

struct doc_page {
    int32_t w, h;
    int first, count;
};

struct doc {
    struct item *items;
    int count, cap;
    char *text;
    uint32_t text_len, text_cap;
    struct doc_page *pages;
    int page_count, page_cap;
    struct picture *pics;
    int pic_count, pic_cap;
    const char *kind; /* "PDF document", ... */
    int *page_obj;    /* PDF: the object number of each page */
    int pages_obj, root_obj, obj_count;
    char error[96];
    /* flow layout */
    int32_t fx, fy, fw, fh, line_h, indent;
    int line_first;
    int align; /* 0 left, 1 centre */
};

static int doc_text_width(int style, int size, const char *s, int n);

static bool doc_grow(void **p, int *cap, int need, size_t elem, int limit)
{
    if (need <= *cap)
        return true;
    if (need > limit)
        return false;
    int n = MAX(need, *cap ? *cap * 2 : 64);
    n = MIN(n, limit);
    void *q = krealloc(*p, (size_t)n * elem);
    if (!q)
        return false;
    *p = q;
    *cap = n;
    return true;
}

static void doc_new_page(struct doc *d, int32_t w, int32_t h)
{
    if (!doc_grow((void **)&d->pages, &d->page_cap, d->page_count + 1, sizeof(struct doc_page), MAX_PAGES))
        return;
    d->pages[d->page_count++] = (struct doc_page){w, h, d->count, 0};
}

static struct item *doc_add(struct doc *d, int kind)
{
    if (!d->page_count)
        doc_new_page(d, A4_W, A4_H);
    if (!doc_grow((void **)&d->items, &d->cap, d->count + 1, sizeof(struct item), MAX_ITEMS))
        return NULL;
    struct item *it = &d->items[d->count++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    d->pages[d->page_count - 1].count = d->count - d->pages[d->page_count - 1].first;
    return it;
}

static uint32_t doc_put_text(struct doc *d, const char *s, size_t n)
{
    if (d->text_len + n + 1 > d->text_cap) {
        uint32_t cap = MAX(d->text_cap * 2, (uint32_t)(d->text_len + n + 1024));
        if (cap > DOC_TEXT_MAX)
            return UINT32_MAX;
        char *t = krealloc(d->text, cap);
        if (!t)
            return UINT32_MAX;
        d->text = t;
        d->text_cap = cap;
    }
    memcpy(d->text + d->text_len, s, n);
    uint32_t at = d->text_len;
    d->text_len += (uint32_t)n;
    d->text[d->text_len] = 0;
    return at;
}

static void doc_text(struct doc *d, int32_t x, int32_t y, int size, int style, uint32_t color, const char *s, size_t n,
                     int32_t w)
{
    size_t blank = 0;
    while (blank < n && (s[blank] == ' ' || s[blank] == '\t'))
        blank++;
    if (blank == n)
        return;
    if (w <= 0) { /* laid out here: no spaces at the ends */
        s += blank;
        n -= blank;
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
            n--;
    }
    uint32_t at = doc_put_text(d, s, n);
    struct item *it = at == UINT32_MAX ? NULL : doc_add(d, IT_TEXT);
    if (!it)
        return;
    it->x = x;
    it->y = y;
    it->size = (uint16_t)CLAMP(size, PT, 400 * PT);
    it->style = (uint8_t)style;
    it->color = color;
    it->text = at;
    it->len = (uint32_t)n;
    it->w = w > 0 ? w : doc_text_width(style, size, s, (int)n);
}

static void doc_shape(struct doc *d, int kind, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    struct item *it = doc_add(d, kind);
    if (it) {
        it->x = x;
        it->y = y;
        it->w = w;
        it->h = h;
        it->color = color;
    }
}

static void doc_free(struct doc *d)
{
    for (int i = 0; i < d->pic_count; i++)
        picture_free(&d->pics[i]);
    kfree(d->pics);
    kfree(d->items);
    kfree(d->text);
    kfree(d->pages);
    kfree(d->page_obj);
    memset(d, 0, sizeof(*d));
}

/* ---- flow layout: paragraphs of runs wrapped into A4 pages ---- */

#define MAX_RUN_WORDS 4096

struct run {
    const char *s;
    int n, style, size;
    uint32_t color;
};

static void flow_start(struct doc *d)
{
    doc_new_page(d, A4_W, A4_H);
    d->fx = MARGIN;
    d->fy = MARGIN;
    d->fw = A4_W - 2 * MARGIN;
    d->fh = A4_H - MARGIN;
}

static void flow_break(struct doc *d)
{
    doc_new_page(d, A4_W, A4_H);
    d->fy = MARGIN;
}

/* lays out one paragraph: runs, wrapped at word boundaries */
static void flow_para(struct doc *d, const struct run *runs, int nruns, int32_t space_after)
{
    struct word {
        const char *s;
        int n, style, size, w;
        uint32_t color;
        bool space; /* a space comes before it */
    } *words = kmalloc(MAX_RUN_WORDS * sizeof(*words));
    if (!words)
        return;
    int count = 0, max_size = 10 * PT;
    bool pending_space = false;
    for (int r = 0; r < nruns; r++) {
        max_size = MAX(max_size, runs[r].size);
        const char *s = runs[r].s, *end = s + runs[r].n;
        while (s < end) {
            if (*s == ' ' || *s == '\t' || *s == '\n') {
                pending_space = true;
                s++;
                continue;
            }
            const char *w = s;
            while (s < end && *s != ' ' && *s != '\t' && *s != '\n')
                s++;
            if (count == MAX_RUN_WORDS)
                break;
            words[count] = (struct word){w, (int)(s - w), runs[r].style, runs[r].size,
                                         doc_text_width(runs[r].style, runs[r].size, w, (int)(s - w)), runs[r].color,
                                         pending_space && count};
            count++;
            pending_space = false;
        }
    }
    if (!count) {
        d->fy += max_size * 6 / 5 + space_after;
        kfree(words);
        return;
    }
    int32_t avail = d->fw - d->indent;
    for (int i = 0; i < count;) {
        int32_t x = 0, line = 0;
        int k = i;
        for (; k < count; k++) {
            int32_t space = words[k].space && k > i ? doc_text_width(words[k].style, words[k].size, " ", 1) : 0;
            if (k > i && x + space + words[k].w > avail)
                break;
            x += space + words[k].w;
            line = MAX(line, words[k].size);
        }
        int32_t lh = line * 6 / 5;
        if (d->fy + lh > d->fh && d->fy > MARGIN)
            flow_break(d);
        int32_t ox = d->fx + d->indent + (d->align == 1 ? (avail - x) / 2 : 0), base = d->fy + line;
        /* words of the same style next to each other become one item */
        for (int j = i; j < k;) {
            int m = j + 1;
            while (m < k && words[m].style == words[j].style && words[m].size == words[j].size &&
                   words[m].color == words[j].color)
                m++;
            char buf[1024];
            int n = 0;
            for (int q = j; q < m; q++) {
                if (q > j && words[q].space && n < (int)sizeof(buf) - 1)
                    buf[n++] = ' ';
                int c = MIN(words[q].n, (int)sizeof(buf) - 1 - n);
                memcpy(buf + n, words[q].s, (size_t)c);
                n += c;
            }
            if (j > i && words[j].space)
                ox += doc_text_width(words[j].style, words[j].size, " ", 1);
            int32_t w = doc_text_width(words[j].style, words[j].size, buf, n);
            doc_text(d, ox, base, words[j].size, words[j].style, words[j].color, buf, (size_t)n, w);
            ox += w;
            j = m;
        }
        d->fy += lh;
        i = k;
    }
    d->fy += space_after;
    kfree(words);
}

/* a table: rows of cells, columns as wide as their text (up to a limit), new pages as needed */
static void flow_table(struct doc *d, char ***cells, int rows, int cols, bool header)
{
    int size = 9 * PT;
    int32_t pad = 4 * PT, widths[64];
    cols = MIN(cols, 64);
    int32_t total = 0;
    for (int c = 0; c < cols; c++) {
        widths[c] = 24 * PT;
        for (int r = 0; r < rows; r++)
            if (cells[r][c])
                widths[c] = MAX(widths[c], doc_text_width(ST_NORMAL, size, cells[r][c], (int)strlen(cells[r][c])) + 2 * pad);
        widths[c] = MIN(widths[c], 200 * PT);
        total += widths[c];
    }
    int32_t page_w = MAX(A4_W, total + 2 * MARGIN / 2);
    if (page_w != A4_W && d->page_count)
        d->pages[d->page_count - 1].w = page_w;
    int32_t rh = size * 2;
    for (int r = 0; r < rows; r++) {
        if (d->fy + rh > d->fh) {
            doc_new_page(d, page_w, A4_H);
            d->fy = MARGIN;
        }
        int32_t x = MARGIN / 2;
        if (header && r == 0)
            doc_shape(d, IT_RECT, x, d->fy, total, rh, 0xffe8ecf2);
        for (int c = 0; c < cols; c++) {
            doc_shape(d, IT_FRAME, x, d->fy, widths[c] + PT / 2, rh + PT / 2, 0xffc8ccd2);
            if (cells[r][c]) {
                char buf[256];
                str_copy(buf, cells[r][c], sizeof(buf));
                int n = (int)strlen(buf);
                while (n > 1 && doc_text_width(ST_NORMAL, size, buf, n) > widths[c] - 2 * pad)
                    n--;
                doc_text(d, x + pad, d->fy + rh - size / 2 - 2 * PT, size, header && r == 0 ? ST_BOLD : ST_NORMAL,
                         0xff000000, buf, (size_t)n, 0);
            }
            x += widths[c];
        }
        d->fy += rh;
    }
    d->fy += 12 * PT;
}

/* the document as plain text: items in order, a new line where the baseline moves */
static char *doc_plain(const struct doc *d, size_t *len)
{
    size_t cap = d->text_len + (size_t)d->count * 2 + (size_t)d->page_count * 2 + 16, n = 0;
    char *out = kmalloc(cap);
    if (!out)
        return NULL;
    for (int p = 0; p < d->page_count; p++) {
        const struct doc_page *pg = &d->pages[p];
        int32_t last_y = INT32_MIN, last_end = 0;
        for (int i = pg->first; i < pg->first + pg->count; i++) {
            const struct item *it = &d->items[i];
            if (it->kind != IT_TEXT)
                continue;
            if (last_y != INT32_MIN) {
                int32_t dy = it->y - last_y;
                if (dy > it->size / 2 || dy < -it->size / 2)
                    out[n++] = '\n';
                else if ((it->x > last_end + it->size / 8 || it->x < last_end - it->size) && out[n - 1] != ' ' &&
                         d->text[it->text] != ' ')
                    out[n++] = ' ';
            }
            memcpy(out + n, d->text + it->text, it->len);
            n += it->len;
            last_y = it->y;
            last_end = it->x + it->w;
        }
        if (n && out[n - 1] != '\n')
            out[n++] = '\n';
        if (p + 1 < d->page_count)
            out[n++] = '\f';
    }
    out[n] = 0;
    *len = n;
    return out;
}
