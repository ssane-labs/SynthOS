/* Office documents: DOCX, XLSX and PPTX (Office Open XML) and OpenDocument (ODT/ODS/ODP), all zip files of XML.
   Only what shows the content is read: paragraphs, runs with bold/italic/size/colour, headings, tables, sheets as
   tables, slides with their text boxes and pictures. */

struct xtag {
    const char *name, *attrs;
    int nlen, alen;
    bool close, empty;
};

/* the next tag after *p; text holds what came before it (still encoded) */
static bool xml_next(const char **p, const char *end, struct xtag *t, const char **text, int *tlen)
{
    const char *s = *p;
    const char *lt = memchr(s, '<', (size_t)(end - s));
    if (!lt)
        return false;
    *text = s;
    *tlen = (int)(lt - s);
    s = lt + 1;
    if (s < end && (*s == '?' || *s == '!')) {
        const char *gt = memchr(s, '>', (size_t)(end - s));
        if (s + 2 < end && !memcmp(s, "!--", 3)) {
            for (gt = s; gt + 2 < end && memcmp(gt, "-->", 3); gt++)
                ;
            gt += 2;
        }
        if (!gt || gt >= end)
            return false;
        *p = gt + 1;
        t->name = "";
        t->nlen = 0;
        t->alen = 0;
        t->attrs = gt;
        t->close = t->empty = true;
        return true;
    }
    t->close = s < end && *s == '/';
    if (t->close)
        s++;
    t->name = s;
    while (s < end && *s != ' ' && *s != '>' && *s != '/' && *s != '\n' && *s != '\t' && *s != '\r')
        s++;
    t->nlen = (int)(s - t->name);
    t->attrs = s;
    char quote = 0;
    while (s < end && (quote || *s != '>')) {
        if (quote && *s == quote)
            quote = 0;
        else if (!quote && (*s == '"' || *s == '\''))
            quote = *s;
        s++;
    }
    if (s >= end)
        return false;
    t->empty = s[-1] == '/';
    t->alen = (int)(s - t->attrs) - (t->empty ? 1 : 0);
    *p = s + 1;
    return true;
}

static bool tag_is(const struct xtag *t, const char *name)
{
    return (int)strlen(name) == t->nlen && !memcmp(t->name, name, (size_t)t->nlen);
}

static int utf8_put(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

/* decodes entities; returns the length written */
static int xml_decode(const char *s, int n, char *out, int cap)
{
    int k = 0;
    for (int i = 0; i < n && k + 4 < cap; i++) {
        if (s[i] != '&') {
            out[k++] = s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ? ' ' : s[i];
            continue;
        }
        const char *semi = memchr(s + i, ';', (size_t)MIN(n - i, 10));
        if (!semi) {
            out[k++] = '&';
            continue;
        }
        const char *e = s + i + 1;
        int len = (int)(semi - e);
        uint32_t c = '?';
        if (len == 3 && !memcmp(e, "amp", 3))
            c = '&';
        else if (len == 2 && !memcmp(e, "lt", 2))
            c = '<';
        else if (len == 2 && !memcmp(e, "gt", 2))
            c = '>';
        else if (len == 4 && !memcmp(e, "quot", 4))
            c = '"';
        else if (len == 4 && !memcmp(e, "apos", 4))
            c = '\'';
        else if (len > 1 && e[0] == '#')
            c = (uint32_t)str_to_long(e + (e[1] == 'x' ? 2 : 1), NULL, e[1] == 'x' ? 16 : 10);
        k += utf8_put(out + k, c ? c : '?');
        i = (int)(semi - s);
    }
    return k;
}

/* the value of an attribute of the tag, decoded */
static bool xml_attr(const struct xtag *t, const char *name, char *out, int cap)
{
    int n = (int)strlen(name);
    const char *s = t->attrs, *end = t->attrs + t->alen;
    while (s + n < end) {
        if ((s == t->attrs || s[-1] == ' ' || s[-1] == '\n' || s[-1] == '\t' || s[-1] == '\r') && !memcmp(s, name, (size_t)n) &&
            s[n] == '=' && s + n + 1 < end) {
            char q = s[n + 1];
            const char *v = s + n + 2, *ve = v;
            while (ve < end && *ve != q)
                ve++;
            int k = xml_decode(v, (int)(ve - v), out, cap);
            out[k] = 0;
            return true;
        }
        s++;
    }
    if (cap)
        out[0] = 0;
    return false;
}

static long xml_attr_num(const struct xtag *t, const char *name, long dflt)
{
    char v[32];
    return xml_attr(t, name, v, sizeof(v)) && v[0] ? str_to_long(v, NULL, 10) : dflt;
}

static uint32_t hex_color(const char *v)
{
    if (strlen(v) != 6)
        return 0xff000000;
    return 0xff000000 | (uint32_t)str_to_long(v, NULL, 16);
}

struct zip_find {
    const char *name;
    struct zip_entry found;
    bool ok;
};

static int zip_find_fn(const struct zip_entry *e, void *ctx)
{
    struct zip_find *f = ctx;
    if (e->name_len == strlen(f->name) && !memcmp(e->name, f->name, e->name_len)) {
        f->found = *e;
        f->ok = true;
        return 1;
    }
    return 0;
}

static char *zip_read(const uint8_t *zip, size_t size, const char *name, size_t *len)
{
    struct zip_find f = {name, {0}, false};
    zip_each(zip, size, zip_find_fn, &f);
    uint8_t *out;
    if (!f.ok || f.found.usize > (64u << 20) || zip_extract(zip, size, &f.found, &out))
        return NULL;
    *len = f.found.usize;
    return (char *)out;
}

/* ---- paragraphs of runs, shared by DOCX, ODF and slides ---- */

#define PARA_TEXT 16384
#define PARA_RUNS 256

struct para {
    char text[PARA_TEXT];
    int len;
    struct run runs[PARA_RUNS];
    int count;
};

static void para_add(struct para *p, const char *raw, int n, int style, int size, uint32_t color, bool decode)
{
    if (p->count == PARA_RUNS || n <= 0)
        return;
    int k = decode ? xml_decode(raw, n, p->text + p->len, PARA_TEXT - p->len)
                   : MIN(n, PARA_TEXT - p->len - 1);
    if (!decode)
        memcpy(p->text + p->len, raw, (size_t)k);
    if (k <= 0)
        return;
    struct run *last = p->count ? &p->runs[p->count - 1] : NULL;
    if (last && last->style == style && last->size == size && last->color == color &&
        last->s + last->n == p->text + p->len)
        last->n += k;
    else
        p->runs[p->count++] = (struct run){p->text + p->len, k, style, size, color};
    p->len += k;
}

static void para_flush(struct doc *d, struct para *p, int32_t after)
{
    flow_para(d, p->runs, p->count, after);
    p->len = p->count = 0;
}

/* ---- DOCX ---- */

static int docx_read(struct doc *d, const uint8_t *zip, size_t size)
{
    size_t len;
    char *xml = zip_read(zip, size, "word/document.xml", &len);
    if (!xml)
        return E_INVAL;
    struct para *p = kzalloc(sizeof(*p));
    if (!p) {
        kfree(xml);
        return E_NOMEM;
    }
    d->kind = "Word document";
    flow_start(d);
    const char *s = xml, *end = xml + len, *text;
    int tlen, psize = 11 * PT, rsize = psize, style = ST_NORMAL, list = 0;
    bool in_ppr = false, in_t = false, page_break = false, bold = false, italic = false, heading = false;
    uint32_t color = 0xff000000;
    struct xtag t;
    char v[64];
    while (xml_next(&s, end, &t, &text, &tlen)) {
        if (in_t && tlen)
            para_add(p, text, tlen, style, rsize, color, true);
        if (tag_is(&t, "w:t")) {
            in_t = !t.close && !t.empty;
        } else if (tag_is(&t, "w:p")) {
            if (!t.close) {
                psize = 11 * PT;
                d->align = 0;
                d->indent = 0;
                heading = false;
                list = 0;
                p->len = p->count = 0;
            }
            if (t.close || t.empty) {
                para_flush(d, p, heading ? 6 * PT : 4 * PT);
                if (page_break)
                    flow_break(d);
                page_break = false;
            }
        } else if (tag_is(&t, "w:pPr")) {
            in_ppr = !t.close && !t.empty;
        } else if (tag_is(&t, "w:pStyle") && xml_attr(&t, "w:val", v, sizeof(v))) {
            if (!str_ncasecmp(v, "Heading", 7) && v[7] >= '1' && v[7] <= '9') {
                psize = (22 - 2 * (v[7] - '1')) * PT;
                heading = true;
            } else if (!str_casecmp(v, "Title")) {
                psize = 28 * PT;
                heading = true;
            } else if (!str_casecmp(v, "Subtitle")) {
                psize = 15 * PT;
            } else if (!str_ncasecmp(v, "List", 4)) {
                list = 1;
            }
        } else if (tag_is(&t, "w:numPr") && !t.close) {
            list = 1;
        } else if (tag_is(&t, "w:jc") && in_ppr && xml_attr(&t, "w:val", v, sizeof(v))) {
            d->align = !strcmp(v, "center") ? 1 : 0;
        } else if (tag_is(&t, "w:r") && !t.close) {
            bold = heading;
            italic = false;
            rsize = psize;
            color = 0xff000000;
            style = bold ? ST_BOLD : ST_NORMAL;
            if (list && !p->len) {
                d->indent = 18 * PT;
                para_add(p, "\xe2\x80\xa2 ", 4, ST_NORMAL, rsize, color, false);
            }
        } else if (!in_ppr && (tag_is(&t, "w:b") || tag_is(&t, "w:i"))) {
            bool on = !xml_attr(&t, "w:val", v, sizeof(v)) || (strcmp(v, "0") && strcmp(v, "false"));
            if (t.name[2] == 'b')
                bold = on;
            else
                italic = on;
            style = bold ? ST_BOLD : italic ? ST_ITALIC : ST_NORMAL;
        } else if (!in_ppr && tag_is(&t, "w:sz")) {
            rsize = (int)xml_attr_num(&t, "w:val", 22) * PT / 2;
        } else if (!in_ppr && tag_is(&t, "w:color") && xml_attr(&t, "w:val", v, sizeof(v))) {
            color = hex_color(v);
        } else if (tag_is(&t, "w:tab")) {
            para_add(p, "    ", 4, style, rsize, color, false);
        } else if (tag_is(&t, "w:br")) {
            if (xml_attr(&t, "w:type", v, sizeof(v)) && !strcmp(v, "page"))
                page_break = true;
            else
                para_add(p, " ", 1, style, rsize, color, false);
        } else if (tag_is(&t, "w:tc") && t.close) {
            para_flush(d, p, 0);
        }
    }
    kfree(p);
    kfree(xml);
    return 0;
}

/* ---- OpenDocument: text:h and text:p of content.xml ---- */

static int odf_read(struct doc *d, const uint8_t *zip, size_t size)
{
    size_t len;
    char *xml = zip_read(zip, size, "content.xml", &len);
    if (!xml)
        return E_INVAL;
    struct para *p = kzalloc(sizeof(*p));
    if (!p) {
        kfree(xml);
        return E_NOMEM;
    }
    d->kind = "OpenDocument";
    flow_start(d);
    const char *s = xml, *end = xml + len, *text;
    int tlen, depth = 0, fsize = 11 * PT, style = ST_NORMAL;
    struct xtag t;
    while (xml_next(&s, end, &t, &text, &tlen)) {
        if (depth && tlen)
            para_add(p, text, tlen, style, fsize, 0xff000000, true);
        bool h = tag_is(&t, "text:h");
        if (h || tag_is(&t, "text:p")) {
            if (!t.close && !t.empty) {
                if (!depth++) {
                    int level = (int)xml_attr_num(&t, "text:outline-level", 1);
                    fsize = h ? (22 - 2 * CLAMP(level - 1, 0, 5)) * PT : 11 * PT;
                    style = h ? ST_BOLD : ST_NORMAL;
                }
            } else if (t.close && depth && !--depth) {
                para_flush(d, p, h ? 6 * PT : 4 * PT);
            } else if (t.empty && !depth) {
                para_flush(d, p, 4 * PT);
            }
        } else if (depth && (tag_is(&t, "text:s") || tag_is(&t, "text:tab"))) {
            para_add(p, "  ", tag_is(&t, "text:s") ? 1 : 2, style, fsize, 0xff000000, false);
        } else if (depth && tag_is(&t, "text:line-break")) {
            para_add(p, " ", 1, style, fsize, 0xff000000, false);
        } else if (tag_is(&t, "table:table-row") && t.close) {
            para_flush(d, p, 2 * PT);
        }
    }
    kfree(p);
    kfree(xml);
    return 0;
}

/* ---- XLSX: every sheet as a table ---- */

#define SHEET_ROWS 1000
#define SHEET_COLS 40

struct strings {
    char **s;
    int count, cap;
};

static void strings_free(struct strings *st)
{
    for (int i = 0; i < st->count; i++)
        kfree(st->s[i]);
    kfree(st->s);
}

static void shared_strings(const uint8_t *zip, size_t size, struct strings *st)
{
    size_t len;
    char *xml = zip_read(zip, size, "xl/sharedStrings.xml", &len);
    if (!xml)
        return;
    const char *s = xml, *end = xml + len, *text;
    int tlen;
    struct xtag t;
    char buf[1024];
    int n = 0;
    bool in_t = false;
    while (xml_next(&s, end, &t, &text, &tlen)) {
        if (in_t && tlen)
            n += xml_decode(text, tlen, buf + n, (int)sizeof(buf) - n);
        if (tag_is(&t, "t"))
            in_t = !t.close && !t.empty;
        else if (tag_is(&t, "si") && !t.close)
            n = 0;
        else if (tag_is(&t, "si") && t.close && doc_grow((void **)&st->s, &st->cap, st->count + 1, sizeof(char *), 1 << 20)) {
            buf[n] = 0;
            st->s[st->count++] = kstrdup(buf);
        }
    }
    kfree(xml);
}

static void sheet_names(const uint8_t *zip, size_t size, struct strings *st)
{
    size_t len;
    char *xml = zip_read(zip, size, "xl/workbook.xml", &len);
    if (!xml)
        return;
    const char *s = xml, *end = xml + len, *text;
    int tlen;
    struct xtag t;
    char name[96];
    while (xml_next(&s, end, &t, &text, &tlen))
        if (tag_is(&t, "sheet") && xml_attr(&t, "name", name, sizeof(name)) &&
            doc_grow((void **)&st->s, &st->cap, st->count + 1, sizeof(char *), 256))
            st->s[st->count++] = kstrdup(name);
    kfree(xml);
}

static void xlsx_sheet(struct doc *d, const char *xml, size_t len, const struct strings *shared, const char *name)
{
    char ***cells = kzalloc(SHEET_ROWS * sizeof(char **));
    if (!cells)
        return;
    int rows = 0, cols = 0, row = -1, col = 0;
    const char *s = xml, *end = xml + len, *text;
    int tlen;
    struct xtag t;
    char type[16], ref[16], buf[512];
    int n = 0;
    bool in_v = false;
    while (xml_next(&s, end, &t, &text, &tlen)) {
        if (in_v && tlen)
            n += xml_decode(text, tlen, buf + n, (int)sizeof(buf) - n - 1);
        if (tag_is(&t, "row") && !t.close) {
            row = (int)xml_attr_num(&t, "r", row + 2) - 1;
        } else if (tag_is(&t, "c") && !t.close) {
            xml_attr(&t, "t", type, sizeof(type));
            if (xml_attr(&t, "r", ref, sizeof(ref))) {
                col = 0;
                for (const char *c = ref; *c >= 'A' && *c <= 'Z'; c++)
                    col = col * 26 + (*c - 'A' + 1);
                col--;
            }
            n = 0;
        } else if ((tag_is(&t, "v") || tag_is(&t, "t")) && !t.empty) {
            in_v = !t.close;
        } else if (tag_is(&t, "c") && (t.close || t.empty)) {
            buf[n] = 0;
            if (n && row >= 0 && row < SHEET_ROWS && col >= 0 && col < SHEET_COLS) {
                const char *value = buf;
                if (!strcmp(type, "s")) {
                    long i = str_to_long(buf, NULL, 10);
                    value = i >= 0 && i < shared->count ? shared->s[i] : "";
                } else if (!strcmp(type, "b")) {
                    value = buf[0] == '1' ? "TRUE" : "FALSE";
                }
                if (!cells[row])
                    cells[row] = kzalloc(SHEET_COLS * sizeof(char *));
                if (cells[row]) {
                    kfree(cells[row][col]);
                    cells[row][col] = kstrdup(value);
                    rows = MAX(rows, row + 1);
                    cols = MAX(cols, col + 1);
                }
            }
            col++;
            n = 0;
        }
    }
    static char *empty_row[SHEET_COLS];
    for (int r = 0; r < rows; r++)
        if (!cells[r])
            cells[r] = empty_row;
    struct run title = {name, (int)strlen(name), ST_BOLD, 16 * PT, 0xff000000};
    flow_para(d, &title, 1, 6 * PT);
    if (rows)
        flow_table(d, cells, rows, cols, true);
    for (int r = 0; r < rows; r++)
        if (cells[r] != empty_row) {
            for (int c = 0; c < SHEET_COLS; c++)
                kfree(cells[r][c]);
            kfree(cells[r]);
        }
    kfree(cells);
}

static int xlsx_read(struct doc *d, const uint8_t *zip, size_t size)
{
    struct strings shared = {0}, names = {0};
    shared_strings(zip, size, &shared);
    sheet_names(zip, size, &names);
    d->kind = "Excel workbook";
    flow_start(d);
    int found = 0;
    for (int i = 1; i <= 64; i++) {
        char path[48];
        size_t len;
        ksnprintf(path, sizeof(path), "xl/worksheets/sheet%d.xml", i);
        char *xml = zip_read(zip, size, path, &len);
        if (!xml)
            continue;
        if (found++)
            flow_break(d);
        char name[32];
        ksnprintf(name, sizeof(name), "Sheet %d", i);
        xlsx_sheet(d, xml, len, &shared, i - 1 < names.count ? names.s[i - 1] : name);
        kfree(xml);
    }
    strings_free(&shared);
    strings_free(&names);
    return found ? 0 : E_INVAL;
}

/* ---- PPTX: a page per slide, text boxes where they are, pictures ---- */

#define EMU 12700 /* per point */

static void rel_target(const uint8_t *zip, size_t size, int slide, const char *id, char *out, int cap)
{
    char path[64];
    size_t len;
    out[0] = 0;
    ksnprintf(path, sizeof(path), "ppt/slides/_rels/slide%d.xml.rels", slide);
    char *xml = zip_read(zip, size, path, &len);
    if (!xml)
        return;
    const char *s = xml, *end = xml + len, *text;
    int tlen;
    struct xtag t;
    char rid[32], target[128];
    while (xml_next(&s, end, &t, &text, &tlen))
        if (tag_is(&t, "Relationship") && xml_attr(&t, "Id", rid, sizeof(rid)) && !strcmp(rid, id) &&
            xml_attr(&t, "Target", target, sizeof(target))) {
            const char *tg = target;
            while (!strncmp(tg, "../", 3))
                tg += 3;
            ksnprintf(out, (size_t)cap, "ppt/%s", tg);
            break;
        }
    kfree(xml);
}

static void pptx_picture(struct doc *d, const uint8_t *zip, size_t size, const char *path, int32_t x, int32_t y,
                         int32_t w, int32_t h)
{
    size_t len;
    char *data = zip_read(zip, size, path, &len);
    if (!data || !doc_grow((void **)&d->pics, &d->pic_cap, d->pic_count + 1, sizeof(struct picture), 512)) {
        kfree(data);
        return;
    }
    struct picture *pic = &d->pics[d->pic_count];
    if (picture_decode((const uint8_t *)data, len, pic) == 0) {
        struct item *it = doc_add(d, IT_IMAGE);
        if (it) {
            *it = (struct item){.kind = IT_IMAGE, .x = x, .y = y, .w = w, .h = h, .text = (uint32_t)d->pic_count};
            d->pic_count++;
        } else {
            picture_free(pic);
        }
    }
    kfree(data);
}

static int pptx_read(struct doc *d, const uint8_t *zip, size_t size)
{
    size_t len;
    int32_t sw = 960 * PT, sh = 540 * PT;
    char *pres = zip_read(zip, size, "ppt/presentation.xml", &len);
    if (pres) {
        const char *s = pres, *end = pres + len, *text;
        int tlen;
        struct xtag t;
        while (xml_next(&s, end, &t, &text, &tlen))
            if (tag_is(&t, "p:sldSz")) {
                sw = (int32_t)(xml_attr_num(&t, "cx", 12192000) * PT / EMU);
                sh = (int32_t)(xml_attr_num(&t, "cy", 6858000) * PT / EMU);
                break;
            }
        kfree(pres);
    }
    struct para *p = kzalloc(sizeof(*p));
    if (!p)
        return E_NOMEM;
    d->kind = "PowerPoint presentation";
    int found = 0;
    for (int n = 1; n <= 500; n++) {
        char path[48];
        ksnprintf(path, sizeof(path), "ppt/slides/slide%d.xml", n);
        char *xml = zip_read(zip, size, path, &len);
        if (!xml) {
            if (n > found + 20)
                break;
            continue;
        }
        found++;
        doc_new_page(d, sw, sh);
        const char *s = xml, *end = xml + len, *text;
        int tlen, size_pt = 18 * PT, style = ST_NORMAL, auto_y = sh / 10;
        int32_t bx = 0, by = 0, bw = 0, bh = 0;
        bool in_t = false, in_shape = false, title = false, in_pic = false, bold = false;
        uint32_t color = 0xff000000;
        struct xtag t;
        char v[64];
        while (xml_next(&s, end, &t, &text, &tlen)) {
            if (in_t && tlen)
                para_add(p, text, tlen, style, size_pt, color, true);
            if ((tag_is(&t, "p:sp") || tag_is(&t, "p:pic")) && !t.close) {
                in_shape = true;
                in_pic = t.name[2] == 'p';
                bx = by = bw = bh = 0;
                title = false;
                d->fx = sw / 12;
                d->fy = auto_y;
                d->fw = sw * 10 / 12;
                d->fh = INT32_MAX / 2;
                d->align = 0;
                d->indent = 0;
            } else if ((tag_is(&t, "p:sp") || tag_is(&t, "p:pic")) && t.close) {
                in_shape = false;
                auto_y = MAX(auto_y, d->fy + 8 * PT);
            } else if (in_shape && tag_is(&t, "a:off")) {
                bx = (int32_t)(xml_attr_num(&t, "x", 0) * PT / EMU);
                by = (int32_t)(xml_attr_num(&t, "y", 0) * PT / EMU);
            } else if (in_shape && tag_is(&t, "a:ext")) {
                bw = (int32_t)(xml_attr_num(&t, "cx", 0) * PT / EMU);
                bh = (int32_t)(xml_attr_num(&t, "cy", 0) * PT / EMU);
                if (bw > 0) {
                    d->fx = bx + 7 * PT;
                    d->fy = by + 4 * PT;
                    d->fw = bw - 14 * PT;
                }
            } else if (in_shape && tag_is(&t, "p:ph")) {
                title = xml_attr(&t, "type", v, sizeof(v)) && (!strcmp(v, "title") || !strcmp(v, "ctrTitle"));
                if (title && !bw) {
                    d->fy = sh / 14;
                    d->align = 1;
                }
            } else if (in_pic && tag_is(&t, "a:blip") && xml_attr(&t, "r:embed", v, sizeof(v))) {
                char media[128];
                rel_target(zip, size, n, v, media, sizeof(media));
                if (media[0] && bw > 0)
                    pptx_picture(d, zip, size, media, bx, by, bw, bh);
            } else if (tag_is(&t, "a:p") && !t.close) {
                p->len = p->count = 0;
            } else if (tag_is(&t, "a:pPr") && xml_attr(&t, "algn", v, sizeof(v))) {
                d->align = !strcmp(v, "ctr") ? 1 : 0;
            } else if (tag_is(&t, "a:p") && t.close) {
                para_flush(d, p, 4 * PT);
            } else if (tag_is(&t, "a:r") && !t.close) {
                size_pt = (title ? 36 : 20) * PT;
                bold = title;
                style = bold ? ST_BOLD : ST_NORMAL;
                color = 0xff000000;
            } else if (tag_is(&t, "a:rPr") || tag_is(&t, "a:endParaRPr")) {
                long sz = xml_attr_num(&t, "sz", 0);
                if (sz > 0 && tag_is(&t, "a:rPr"))
                    size_pt = (int)(sz * PT / 100);
                if (xml_attr(&t, "b", v, sizeof(v)))
                    bold = v[0] == '1';
                style = bold ? ST_BOLD : ST_NORMAL;
            } else if (tag_is(&t, "a:srgbClr") && xml_attr(&t, "val", v, sizeof(v)) && in_shape) {
                color = hex_color(v);
            } else if (tag_is(&t, "a:t")) {
                in_t = !t.close && !t.empty;
            } else if (tag_is(&t, "a:br")) {
                para_add(p, " ", 1, style, size_pt, color, false);
            }
        }
        kfree(xml);
    }
    kfree(p);
    return found ? 0 : E_INVAL;
}

static int office_read(struct doc *d, const uint8_t *data, size_t size)
{
    size_t len;
    char *mime = zip_read(data, size, "mimetype", &len);
    if (mime) {
        kfree(mime);
        return odf_read(d, data, size);
    }
    struct zip_find f = {"word/document.xml", {0}, false};
    zip_each(data, size, zip_find_fn, &f);
    if (f.ok)
        return docx_read(d, data, size);
    f = (struct zip_find){"xl/workbook.xml", {0}, false};
    zip_each(data, size, zip_find_fn, &f);
    if (f.ok)
        return xlsx_read(d, data, size);
    f = (struct zip_find){"ppt/presentation.xml", {0}, false};
    zip_each(data, size, zip_find_fn, &f);
    if (f.ok)
        return pptx_read(d, data, size);
    return E_INVAL;
}
