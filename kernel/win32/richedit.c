#include <synth/config.h>
#if CONFIG_GUI
#include <synth/fmt.h>
#include <synth/string.h>

#include "w32gui.h"
#include "win64.h"

/* RichEdit: a multi-line edit control that takes RTF; formatting is dropped, the text is kept */
enum {
    EM_EXGETSEL = 0x434,
    EM_EXLIMITTEXT = 0x435,
    EM_EXSETSEL = 0x437,
    EM_FINDTEXT = 0x438,
    EM_FORMATRANGE = 0x439,
    EM_GETEVENTMASK = 0x43b,
    EM_GETOLEINTERFACE = 0x43c,
    EM_HIDESELECTION = 0x43f,
    EM_REQUESTRESIZE = 0x441,
    EM_SETBKGNDCOLOR = 0x443,
    EM_SETCHARFORMAT = 0x444,
    EM_SETEVENTMASK = 0x445,
    EM_SETPARAFORMAT = 0x447,
    EM_SETTARGETDEVICE = 0x448,
    EM_STREAMIN = 0x449,
    EM_STREAMOUT = 0x44a,
    EM_GETTEXTRANGE = 0x44b,
    EM_SETOPTIONS = 0x44d,
    EM_GETOPTIONS = 0x44e,
    EM_FINDTEXTEX = 0x44f,
    EM_AUTOURLDETECT = 0x45b,
    EM_GETTEXTEX = 0x45e,
    EM_GETTEXTLENGTHEX = 0x45f,
    EM_SETTEXTEX = 0x461,
    EM_SETLANGOPTIONS = 0x478,
    EM_GETLANGOPTIONS = 0x479,
    EM_FINDTEXTW = 0x47b,
    EM_FINDTEXTEXW = 0x47c,
    FR_DOWN = 1,
    FR_WHOLEWORD = 2,
    FR_MATCHCASE = 4,
    EM_SETTYPOGRAPHYOPTIONS = 0x4ca,
    EM_SETEDITSTYLE = 0x4cc,
    EM_GETEDITSTYLE = 0x4cd,
    EM_SETZOOM = 0x4e1,
    SF_TEXT = 0x1,
    SF_RTF = 0x2,
    SF_UNICODE = 0x10,
    SF_USECODEPAGE = 0x20,
    SFF_SELECTION = 0x8000,
    RTF_MAX = 8 << 20,
};

typedef struct {
    uintptr_t cookie;
    DWORD error;
    void *callback;
} EDITSTREAM;

struct sbuf {
    char *p;
    size_t n, cap;
    bool failed;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
    if (b->failed)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->n + n + 256);
        char *p = w32_realloc(b->p, cap);
        if (!p) {
            b->failed = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_char(struct sbuf *b, uint32_t c)
{
    char u[4];
    size_t n;
    if (c < 0x80) {
        u[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (char)(0xc0 | c >> 6);
        u[1] = (char)(0x80 | (c & 0x3f));
        n = 2;
    } else {
        u[0] = (char)(0xe0 | c >> 12);
        u[1] = (char)(0x80 | (c >> 6 & 0x3f));
        u[2] = (char)(0x80 | (c & 0x3f));
        n = 3;
    }
    sb_put(b, u, n);
}

static void sb_ansi(struct sbuf *b, uint8_t c, int codepage)
{
    if (c < 0x80) {
        sb_char(b, c);
        return;
    }
    WCHAR w = '?';
    w32_cp_to_wide((const char *)&c, 1, &w, 1, codepage);
    sb_char(b, w);
}

static bool skipped_destination(const char *word)
{
    static const char *const names[] = {"fonttbl",  "colortbl", "stylesheet",  "info",       "pict",      "object",
                                        "header",   "footer",   "headerl",     "headerr",    "footerl",   "footerr",
                                        "footnote", "themedata", "colorschememapping", "latentstyles", "datastore",
                                        "xmlnstbl", "listtable", "listoverridetable", "rsidtbl", "generator",
                                        "mmathPr",  "pgdsctbl", "fldinst",     "filetbl",    "revtbl",    "bkmkstart",
                                        "bkmkend",  "nonshppict"};
    for (size_t i = 0; i < ARRAY_SIZE(names); i++)
        if (!strcmp(word, names[i]))
            return true;
    return false;
}

/* what the RTF says about the look of its text: the color, size, weight and font in force at the first character */
struct rtf_look {
    uint32_t colors[16];
    int ncolors, cf, fs, bold, font, tab;
    char fonts[4][32];
    bool seen;
    struct rich_style style;
};

static void rtf_colortbl(const char *s, size_t n, size_t *i, struct rtf_look *look)
{
    uint32_t c = 0;
    bool any = false;
    while (*i < n && s[*i] != '}') {
        if (s[*i] == ';') {
            if (look->ncolors < 16)
                look->colors[look->ncolors++] = any ? c : 0xffffffffu;
            c = 0;
            any = false;
            (*i)++;
        } else if (s[*i] == '\\') {
            size_t at = ++*i;
            while (*i < n && s[*i] >= 'a' && s[*i] <= 'z')
                (*i)++;
            unsigned v = 0;
            while (*i < n && s[*i] >= '0' && s[*i] <= '9')
                v = v * 10 + (unsigned)(s[(*i)++] - '0');
            v &= 0xff;
            if (!strncmp(s + at, "red", 3))
                c |= v, any = true;
            else if (!strncmp(s + at, "green", 5))
                c |= v << 8, any = true;
            else if (!strncmp(s + at, "blue", 4))
                c |= v << 16, any = true;
        } else {
            (*i)++;
        }
    }
}

/* {\fonttbl{\f0\fswiss Tahoma;}...}: the first four face names */
static void rtf_fonttbl(const char *s, size_t n, size_t *i, struct rtf_look *look)
{
    int depth = 1, id = -1;
    while (*i < n && depth) {
        char c = s[*i];
        if (c == '{') {
            depth++, (*i)++;
        } else if (c == '}') {
            depth--;
            if (depth)
                (*i)++;
        } else if (c == '\\') {
            (*i)++;
            if (*i < n && s[*i] == 'f' && *i + 1 < n && s[*i + 1] >= '0' && s[*i + 1] <= '9') {
                id = 0;
                for ((*i)++; *i < n && s[*i] >= '0' && s[*i] <= '9'; (*i)++)
                    id = id * 10 + (s[*i] - '0');
            } else {
                while (*i < n && s[*i] != '\\' && s[*i] != ' ' && s[*i] != '{' && s[*i] != '}' && s[*i] != ';')
                    (*i)++;
            }
            if (*i < n && s[*i] == ' ')
                (*i)++;
        } else if (id >= 0 && id < 4 && depth == 2) {
            size_t k = 0;
            while (*i < n && s[*i] != ';' && s[*i] != '}' && s[*i] != '{' && s[*i] != '\\') {
                if (k + 1 < sizeof(look->fonts[0]))
                    look->fonts[id][k++] = s[*i];
                (*i)++;
            }
            look->fonts[id][k] = 0;
            id = -1;
        } else {
            (*i)++;
        }
    }
}

static void rtf_mark(struct rtf_look *look)
{
    if (!look || look->seen)
        return;
    look->seen = true;
    look->style.color = look->cf > 0 && look->cf < look->ncolors ? look->colors[look->cf] : 0xffffffffu;
    look->style.has_color = look->style.color != 0xffffffffu;
    look->style.twips = look->fs ? look->fs * 10 : 0;
    look->style.weight = look->bold ? 700 : 400;
    look->style.tab = look->tab;
    str_copy(look->style.face, look->font >= 0 && look->font < 4 ? look->fonts[look->font] : "", sizeof(look->style.face));
}

static void rtf_to_text(const char *s, size_t n, struct sbuf *out, struct rtf_look *look)
{
    int depth = 0, skip = -1, pending = 0, codepage = 1252;
    int uc[32] = {1};
    for (size_t i = 0; i < n;) {
        char c = s[i];
        if (c == '{') {
            if (++depth < (int)ARRAY_SIZE(uc))
                uc[depth] = uc[depth - 1];
            i++;
            continue;
        }
        if (c == '}') {
            if (skip >= 0 && depth <= skip)
                skip = -1;
            if (depth)
                depth--;
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c != '\\') {
            i++;
            if (skip >= 0)
                continue;
            if (pending) {
                pending--;
                continue;
            }
            if (c != ' ' || out->n)
                rtf_mark(look);
            sb_ansi(out, (uint8_t)c, codepage);
            continue;
        }
        if (++i >= n)
            break;
        c = s[i];
        if (c == '\'') {
            unsigned v = 0;
            for (int k = 1; k <= 2 && i + k < n; k++) {
                char h = s[i + k];
                v = v * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
            }
            i += 3;
            if (skip >= 0)
                continue;
            if (pending)
                pending--;
            else
                sb_ansi(out, (uint8_t)v, codepage);
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            i++;
            if (c == '*' && skip < 0)
                skip = depth;
            if (skip >= 0)
                continue;
            if (c == '~')
                sb_char(out, 0xa0);
            else if (c == '_')
                sb_char(out, '-');
            else if (c == '{' || c == '}' || c == '\\')
                sb_char(out, (uint8_t)c);
            else if (c == '\r' || c == '\n')
                sb_put(out, "\r\n", 2);
            continue;
        }
        char word[32];
        size_t wn = 0;
        while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
            if (wn + 1 < sizeof(word))
                word[wn++] = s[i];
            i++;
        }
        word[wn] = 0;
        bool neg = false, has = false;
        long param = 0;
        if (i < n && s[i] == '-') {
            neg = true;
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            param = param * 10 + (s[i++] - '0');
            has = true;
        }
        if (neg)
            param = -param;
        if (i < n && s[i] == ' ')
            i++;
        if (skip >= 0)
            continue;
        if (look && !strcmp(word, "colortbl")) {
            rtf_colortbl(s, n, &i, look);
        } else if (look && !strcmp(word, "fonttbl")) {
            rtf_fonttbl(s, n, &i, look);
        } else if (skipped_destination(word)) {
            skip = depth;
        } else if (look && !strcmp(word, "tx") && has && !look->tab) {
            look->tab = (int)param;
        } else if (look && (!strcmp(word, "cf") || !strcmp(word, "fs") || !strcmp(word, "b") || !strcmp(word, "f"))) {
            int *field = word[0] == 'c' ? &look->cf : word[0] == 'b' ? &look->bold : word[1] ? &look->fs : &look->font;
            *field = has ? (int)param : word[0] == 'b';
        } else if (!strcmp(word, "par") || !strcmp(word, "line") || !strcmp(word, "sect") ||
                   !strcmp(word, "page") || !strcmp(word, "row")) {
            sb_put(out, "\r\n", 2);
        } else if (!strcmp(word, "tab") || !strcmp(word, "cell")) {
            sb_char(out, '\t');
        } else if (!strcmp(word, "u") && has) {
            sb_char(out, (uint32_t)(param < 0 ? param + 65536 : param) & 0xffff);
            pending = depth < (int)ARRAY_SIZE(uc) ? uc[depth] : 1;
        } else if (!strcmp(word, "uc") && depth < (int)ARRAY_SIZE(uc)) {
            uc[depth] = (int)param;
        } else if (!strcmp(word, "ansicpg") && has) {
            codepage = (int)param;
        } else if (!strcmp(word, "emdash")) {
            sb_char(out, 0x2014);
        } else if (!strcmp(word, "endash")) {
            sb_char(out, 0x2013);
        } else if (!strcmp(word, "bullet")) {
            sb_char(out, 0x2022);
        } else if (!strcmp(word, "lquote") || !strcmp(word, "rquote")) {
            sb_char(out, !strcmp(word, "lquote") ? 0x2018 : 0x2019);
        } else if (!strcmp(word, "ldblquote") || !strcmp(word, "rdblquote")) {
            sb_char(out, !strcmp(word, "ldblquote") ? 0x201c : 0x201d);
        }
    }
}

/* the text a stream or EM_SETTEXTEX gives, as UTF-8 for the control */
static char *rich_text(const char *data, size_t n, bool rtf, bool unicode, int codepage, struct rtf_look *look)
{
    struct sbuf out = {0};
    /* like RichEdit, SF_RTF data that is not RTF is read as plain text */
    rtf = n >= 5 && !memcmp(data, "{\\rtf", 5);
    if (!rtf && !unicode && n >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) {
        data += 3;
        n -= 3;
        codepage = 65001;
    }
    if (!rtf && !unicode && n >= 2 && (uint8_t)data[0] == 0xff && (uint8_t)data[1] == 0xfe) {
        data += 2;
        n -= 2;
        unicode = true;
    }
    if (rtf) {
        rtf_to_text(data, n, &out, look);
    } else if (unicode) {
        const WCHAR *w = (const WCHAR *)data;
        for (size_t i = 0; i < n / 2 && w[i]; i++)
            if (w[i] == '\r' && (i + 1 >= n / 2 || w[i + 1] != '\n'))
                sb_put(&out, "\r\n", 2);
            else
                sb_char(&out, w[i]);
    } else {
        for (size_t i = 0; i < n && data[i]; i++)
            if (data[i] == '\r' && (i + 1 >= n || data[i + 1] != '\n'))
                sb_put(&out, "\r\n", 2);
            else if (data[i] == '\n' && (!i || data[i - 1] != '\r'))
                sb_put(&out, "\r\n", 2);
            else if (codepage == 65001 || (uint8_t)data[i] < 0x80)
                sb_put(&out, data + i, 1);
            else
                sb_ansi(&out, (uint8_t)data[i], codepage);
    }
    if (!out.p && !out.failed)
        sb_put(&out, "", 0);
    if (out.failed) {
        w32_free(out.p);
        return NULL;
    }
    return out.p;
}

static void set_text(struct w32_window *w, char *text, bool selection)
{
    if (!text)
        return;
    if (selection && w->text) {
        size_t len = strlen(w->text), at = MIN(w->caret, len), add = strlen(text);
        char *joined = w32_alloc(len + add + 1);
        if (joined) {
            memcpy(joined, w->text, at);
            memcpy(joined + at, text, add);
            memcpy(joined + at + add, w->text + at, len - at + 1);
            w32_free(w->text);
            w->text = joined;
            w->caret = at + add;
        }
        w32_free(text);
    } else {
        w32_free(w->text);
        w->text = text;
        w->caret = 0;
        w->scroll = 0;
    }
    w32_invalidate(w, NULL, true);
}

/* the program's callback; a Win64 EDITSTREAM is {cookie, error, pad, callback} with 8-byte cookie and pointer */
static DWORD stream_call(EDITSTREAM *es, char *buf, LONG n, LONG *got)
{
#ifdef __x86_64__
    if (w32->win64) {
        uint64_t fn, args[4] = {0, (uintptr_t)buf, (uint64_t)n, (uintptr_t)got};
        memcpy(&args[0], es, 8);
        memcpy(&fn, (uint8_t *)es + 16, 8);
        return (DWORD)w64_call(fn, 4, args);
    }
#endif
    return W32_CALL(DWORD(WINAPI *)(uintptr_t, void *, LONG, LONG *), es->callback, es->cookie, buf, n, got);
}

static void stream_error(EDITSTREAM *es, DWORD error)
{
#ifdef __x86_64__
    if (w32->win64) {
        memcpy((uint8_t *)es + 8, &error, 4);
        return;
    }
#endif
    es->error = error;
}

static LRESULT stream_in(struct w32_window *w, WPARAM flags, EDITSTREAM *es)
{
    uint64_t fn = 0;
    if (es)
        memcpy(&fn, (uint8_t *)es + (w32->win64 ? 16 : 8), w32->win64 ? 8 : 4);
    if (!es || !fn)
        return 0;
    struct sbuf raw = {0};
    char *buf = w32_alloc(4096);
    LONG *got = w32_alloc(sizeof(LONG));
    if (!buf || !got) {
        w32_free(buf);
        w32_free(got);
        return 0;
    }
    stream_error(es, 0);
    while (raw.n < RTF_MAX && !raw.failed) {
        *got = 0;
        DWORD r = stream_call(es, buf, 4096, got);
        if (r) {
            stream_error(es, r);
            break;
        }
        if (*got <= 0)
            break;
        sb_put(&raw, buf, (size_t)MIN(*got, 4096));
    }
    w32_free(buf);
    w32_free(got);
    int codepage = flags & SF_USECODEPAGE ? (int)(flags >> 16) : w32_acp();
    struct rtf_look look = {.font = 0};
    char *text = raw.p ? rich_text(raw.p, raw.n, flags & SF_RTF, (flags & SF_UNICODE) || codepage == 1200, codepage,
                                   &look)
                       : NULL;
    if (look.seen && !(flags & SFF_SELECTION))
        w->rich_style = look.style;
    w32_free(raw.p);
    if (!text)
        text = w32_zalloc(1);
    LRESULT count = text ? (LRESULT)strlen(text) : 0;
    set_text(w, text, flags & SFF_SELECTION);
    return count;
}

/* the text as RTF, UTF-16 or the code page, handed to the program's callback in pieces */
static LRESULT stream_out(struct w32_window *w, WPARAM flags, EDITSTREAM *es)
{
    uint64_t fn = 0;
    if (es)
        memcpy(&fn, (uint8_t *)es + (w32->win64 ? 16 : 8), w32->win64 ? 8 : 4);
    if (!es || !fn)
        return 0;
    struct sbuf out = {0};
    const char *t = w->text ? w->text : "";
    int codepage = flags & SF_USECODEPAGE ? (int)(flags >> 16) : w32_acp();
    bool rtf = flags & SF_RTF, wide = !rtf && ((flags & SF_UNICODE) || codepage == 1200);
    if (rtf)
        sb_put(&out, "{\\rtf1\\ansi\\deff0 ", 18);
    LRESULT chars = 0;
    while (*t) {
        uint32_t c = utf8_next(&t);
        chars++;
        if (wide) {
            WCHAR u[2] = {(WCHAR)c, 0};
            int n = 1;
            if (c > 0xffff) {
                u[0] = (WCHAR)(0xd800 + ((c - 0x10000) >> 10));
                u[1] = (WCHAR)(0xdc00 + ((c - 0x10000) & 0x3ff));
                n = 2;
            }
            sb_put(&out, (const char *)u, (size_t)n * 2);
        } else if (rtf) {
            char e[24];
            if (c == '\\' || c == '{' || c == '}')
                ksnprintf(e, sizeof(e), "\\%c", (char)c);
            else if (c == '\n')
                str_copy(e, "\\par\n", sizeof(e));
            else if (c == '\r')
                e[0] = 0;
            else if (c < 0x80)
                ksnprintf(e, sizeof(e), "%c", (char)c);
            else
                ksnprintf(e, sizeof(e), "\\u%d?", c > 0xffff ? '?' : (int)(int16_t)c);
            sb_put(&out, e, strlen(e));
        } else {
            WCHAR u = c > 0xffff ? '?' : (WCHAR)c;
            char b[4];
            size_t n = w32_wide_to_cp(&u, 1, b, sizeof(b), codepage);
            sb_put(&out, b, n);
        }
    }
    if (rtf)
        sb_put(&out, "}", 1);
    char *buf = w32_alloc(4096);
    LONG *done = w32_alloc(sizeof(LONG));
    stream_error(es, 0);
    for (size_t at = 0; buf && done && at < out.n;) {
        LONG n = (LONG)MIN(out.n - at, 4096);
        memcpy(buf, out.p + at, (size_t)n);
        *done = 0;
        DWORD r = stream_call(es, buf, n, done);
        if (r) {
            stream_error(es, r);
            break;
        }
        if (*done <= 0)
            break;
        at += (size_t)*done;
    }
    w32_free(buf);
    w32_free(done);
    w32_free(out.p);
    return chars;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++)
        n += ((uint8_t)*s & 0xc0) != 0x80;
    return n;
}

/* the byte offset of character position cp (-1 or past the end: the end) */
static size_t byte_at(const char *s, LONG cp)
{
    size_t i = 0;
    for (LONG n = 0; s[i] && (cp < 0 || n < cp); n++)
        for (i++; ((uint8_t)s[i] & 0xc0) == 0x80; i++)
            ;
    return i;
}

static size_t chars_in(const char *s, size_t bytes)
{
    size_t n = 0;
    for (size_t i = 0; i < bytes; i++)
        n += ((uint8_t)s[i] & 0xc0) != 0x80;
    return n;
}

static void *struct_ptr(const uint8_t *p, size_t off)
{
    uint64_t v = 0;
    memcpy(&v, p + off, w32->win64 ? 8 : 4);
    return (void *)(uintptr_t)v;
}

static bool word_char(char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_' || (uint8_t)c >= 0x80;
}

static bool matches_at(const char *t, size_t at, const char *n, size_t len, WPARAM flags)
{
    for (size_t k = 0; k < len; k++) {
        char a = t[at + k], b = n[k];
        if (!a)
            return false;
        if (!(flags & FR_MATCHCASE) && a >= 'A' && a <= 'Z')
            a += 32;
        if (!(flags & FR_MATCHCASE) && b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
    }
    return !(flags & FR_WHOLEWORD) || ((!at || !word_char(t[at - 1])) && !word_char(t[at + len]));
}

/* FINDTEXT(EX): CHARRANGE to search, the text, then (EX) the range found; -1 when it is not there */
static LRESULT find_text(struct w32_window *w, WPARAM flags, uint8_t *ft, bool wide, bool ex)
{
    if (!ft || !w->text)
        return -1;
    const void *what = struct_ptr(ft, 8);
    if (!what)
        return -1;
    char *needle;
    if (wide) {
        needle = w32_narrow(what);
    } else {
        WCHAR *tmp = w32_alloc((strlen(what) + 1) * sizeof(WCHAR));
        size_t n = tmp ? w32_cp_to_wide(what, -1, tmp, strlen(what) + 1, w32_acp()) : 0;
        if (tmp)
            tmp[MIN(n, strlen(what))] = 0;
        needle = tmp ? w32_narrow(tmp) : NULL;
        w32_free(tmp);
    }
    if (!needle)
        return -1;
    LONG lo, hi;
    memcpy(&lo, ft, 4);
    memcpy(&hi, ft + 4, 4);
    size_t len = strlen(needle), from = byte_at(w->text, lo), to = byte_at(w->text, hi), found = (size_t)-1;
    if (len && (flags & FR_DOWN)) {
        for (size_t i = from; i + len <= to && found == (size_t)-1; i++)
            if (matches_at(w->text, i, needle, len, flags))
                found = i;
    } else if (len) {
        size_t stop = hi < 0 ? 0 : to;
        for (size_t i = from + 1; i-- > stop && found == (size_t)-1;)
            if (matches_at(w->text, i, needle, len, flags))
                found = i;
    }
    w32_free(needle);
    if (found == (size_t)-1)
        return -1;
    LONG at = (LONG)chars_in(w->text, found), end = at + (LONG)chars_in(w->text + found, len);
    if (ex) {
        size_t off = 8 + (w32->win64 ? 8 : 4);
        memcpy(ft + off, &at, 4);
        memcpy(ft + off + 4, &end, 4);
    }
    return at;
}

/* TEXTRANGE: the characters in a CHARRANGE copied out, UTF-16 for a Unicode window */
static LRESULT text_range(struct w32_window *w, uint8_t *tr)
{
    if (!tr || !w->text)
        return 0;
    void *out = struct_ptr(tr, 8);
    LONG lo, hi;
    memcpy(&lo, tr, 4);
    memcpy(&hi, tr + 4, 4);
    if (!out)
        return 0;
    size_t a = byte_at(w->text, lo), b = byte_at(w->text, hi);
    if (b < a)
        b = a;
    char *piece = w32_alloc(b - a + 1);
    if (!piece)
        return 0;
    memcpy(piece, w->text + a, b - a);
    piece[b - a] = 0;
    LRESULT n;
    if (w->unicode || w32_caller_wide) {
        size_t c = w32_cp_to_wide(piece, -1, NULL, 0, 1);
        w32_cp_to_wide(piece, -1, out, c + 1, 1);
        ((WCHAR *)out)[c] = 0;
        n = (LRESULT)c;
    } else {
        WCHAR *tmp = w32_widen(piece);
        size_t c = tmp ? w32_wide_to_cp(tmp, -1, NULL, 0, w32_acp()) : 0;
        if (tmp)
            w32_wide_to_cp(tmp, -1, out, c + 1, w32_acp());
        ((char *)out)[c] = 0;
        w32_free(tmp);
        n = (LRESULT)c;
    }
    w32_free(piece);
    return n;
}

/* CHARFORMAT: the one look this control keeps for all its text */
static void set_char_format(struct w32_window *w, const uint8_t *cf)
{
    uint32_t mask, effects, height, color;
    memcpy(&mask, cf + 4, 4);
    memcpy(&effects, cf + 8, 4);
    memcpy(&height, cf + 12, 4);
    memcpy(&color, cf + 20, 4);
    struct rich_style *st = &w->rich_style;
    if (mask & 1)
        st->weight = effects & 1 ? 700 : 400;
    if (mask & 0x80000000u)
        st->twips = (int)height;
    if (mask & 0x40000000u) {
        st->has_color = !(effects & 0x40000000u);
        st->color = color;
    }
    if (mask & 0x20000000u) {
        if (w->unicode || w32_caller_wide) {
            WCHAR face[32];
            memcpy(face, cf + 26, sizeof(face));
            face[31] = 0;
            char *n = w32_narrow(face);
            str_copy(st->face, n ? n : "", sizeof(st->face));
            w32_free(n);
        } else {
            memcpy(st->face, cf + 26, 31);
            st->face[31] = 0;
        }
    }
}

typedef HANDLE(WINAPI *font_fn)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                                const char *);

/* the text between bytes a and b laid out in the control's look on dc inside box (pixels); box comes back as the
   space it takes, and with render it is drawn */
static bool lay_text(struct w32_window *w, HANDLE dc, size_t a, size_t b, LONG box[4], bool wrap, bool render)
{
    font_fn create = (font_fn)w32_builtin_lookup("gdi32.dll", "CreateFontA");
    HANDLE(WINAPI * select)(HANDLE, HANDLE) = (HANDLE(WINAPI *)(HANDLE, HANDLE))w32_builtin_lookup("gdi32.dll", "SelectObject");
    BOOL(WINAPI * del)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "DeleteObject");
    int(WINAPI * caps)(HANDLE, int) = (int(WINAPI *)(HANDLE, int))w32_builtin_lookup("gdi32.dll", "GetDeviceCaps");
    COLORREF(WINAPI * text_color)(HANDLE, COLORREF) =
        (COLORREF(WINAPI *)(HANDLE, COLORREF))w32_builtin_lookup("gdi32.dll", "SetTextColor");
    int(WINAPI * bk_mode)(HANDLE, int) = (int(WINAPI *)(HANDLE, int))w32_builtin_lookup("gdi32.dll", "SetBkMode");
    int(WINAPI * draw)(HANDLE, const WCHAR *, int, RECT *, UINT, int *) =
        (int(WINAPI *)(HANDLE, const WCHAR *, int, RECT *, UINT, int *))w32_builtin_lookup("user32.dll", "DrawTextExW");
    BOOL(WINAPI * metrics)(HANDLE, uint8_t *) = (BOOL(WINAPI *)(HANDLE, uint8_t *))w32_builtin_lookup("gdi32.dll", "GetTextMetricsW");
    if (!create || !select || !del || !caps || !text_color || !bk_mode || !draw || !metrics || b < a)
        return false;
    int dpy = caps(dc, 90);
    const struct rich_style *st = &w->rich_style;
    int px = (st->twips > 0 ? st->twips : 200) * (dpy > 0 ? dpy : 96) / 1440;
    char *face = w32_alloc(32), *piece = w32_alloc(b - a + 1);
    RECT *r = w32_alloc(sizeof(RECT));
    int *params = w32_zalloc(5 * sizeof(int));
    uint8_t *tm = w32_zalloc(64);
    WCHAR *text = NULL;
    if (face && piece) {
        str_copy(face, st->face[0] ? st->face : "Segoe UI", 32);
        memcpy(piece, w->text + a, b - a);
        piece[b - a] = 0;
        text = w32_widen(piece);
    }
    w32_free(piece);
    if (!text || !r || !params || !tm) {
        w32_free(face);
        w32_free(text);
        w32_free(r);
        w32_free(params);
        w32_free(tm);
        return false;
    }
    HANDLE font = create(-MAX(px, 1), 0, 0, 0, st->weight ? st->weight : 400, 0, 0, 0, 1, 0, 0, 0, 0, face);
    HANDLE old = font ? select(dc, font) : NULL;
    UINT flags = (wrap ? 0x10 : 0) | 0x40 | 0x800; /* DT_WORDBREAK, DT_EXPANDTABS, DT_NOPREFIX */
    LONG avg = 0;
    if (metrics(dc, tm))
        memcpy(&avg, tm + 20, 4);
    params[0] = 5 * sizeof(int);
    params[1] = st->tab > 0 && avg > 0 ? MAX(1, (int)((st->tab * (dpy > 0 ? dpy : 96) / 1440 + avg - 1) / avg)) : 0;
    *r = (RECT){box[0], box[1], box[2], box[3]};
    draw(dc, text, -1, r, flags | 0x400, params); /* DT_CALCRECT */
    LONG right = r->right, bottom = r->bottom;
    if (render) {
        *r = (RECT){box[0], box[1], box[2], box[3]};
        text_color(dc, st->has_color ? st->color : w32_colorref(style.text));
        bk_mode(dc, 1);
        draw(dc, text, -1, r, flags, params);
    }
    box[2] = right;
    box[3] = bottom;
    if (old)
        select(dc, old);
    if (font)
        del(font);
    w32_free(text);
    w32_free(face);
    w32_free(r);
    w32_free(params);
    w32_free(tm);
    return true;
}

/* FORMATRANGE: the text from chrg.cpMin drawn into hdc inside rc (twips); wParam 0 only measures. rc.bottom becomes
   the bottom of the text, and the result is the first character that did not fit (all of them do). */
static LRESULT format_range(struct w32_window *w, WPARAM render, uint8_t *fr)
{
    if (!fr || !w->text)
        return 0;
    size_t p = w32->win64 ? 8 : 4;
    HANDLE dc = struct_ptr(fr, 0);
    LONG rc[4], from, to;
    memcpy(rc, fr + 2 * p, 16);
    memcpy(&from, fr + 2 * p + 32, 4);
    memcpy(&to, fr + 2 * p + 36, 4);
    size_t a = byte_at(w->text, from), b = byte_at(w->text, to);
    LONG total = (LONG)utf8_chars(w->text);
    int(WINAPI * caps)(HANDLE, int) = (int(WINAPI *)(HANDLE, int))w32_builtin_lookup("gdi32.dll", "GetDeviceCaps");
    if (!dc || b <= a || !caps)
        return total;
    int dpx = caps(dc, 88), dpy = caps(dc, 90);
    dpx = dpx > 0 ? dpx : 96;
    dpy = dpy > 0 ? dpy : 96;
    LONG box[4] = {rc[0] * dpx / 1440, rc[1] * dpy / 1440, rc[2] * dpx / 1440, rc[3] * dpy / 1440};
    LONG limit = box[3];
    if (lay_text(w, dc, a, b, box, true, render)) {
        LONG bottom = (MIN(box[3], limit) - rc[1] * dpy / 1440) * 1440 / dpy + rc[1];
        memcpy(fr + 2 * p + 12, &bottom, 4);
    }
    return total;
}

/* EM_REQUESTRESIZE: EN_REQUESTRESIZE tells the parent the size the whole text needs */
static void request_resize(struct w32_window *w)
{
    HANDLE(WINAPI * mem_dc)(HANDLE) = (HANDLE(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "CreateCompatibleDC");
    BOOL(WINAPI * del_dc)(HANDLE) = (BOOL(WINAPI *)(HANDLE))w32_builtin_lookup("gdi32.dll", "DeleteDC");
    if (!w->parent || !w->text || !mem_dc || !del_dc)
        return;
    HANDLE dc = mem_dc(NULL);
    LONG box[4] = {0, 0, 32767, 32767};
    bool ok = dc && lay_text(w, dc, 0, strlen(w->text), box, false, false);
    if (dc)
        del_dc(dc);
    if (!ok)
        return;
    size_t p = w32->win64 ? 8 : 4, hs = 3 * p;
    uint8_t *nm = w32_zalloc(hs + 16);
    if (!nm)
        return;
    uint64_t hwnd = (uintptr_t)w, id = (uint64_t)w->id;
    int code = 0x701;
    memcpy(nm, &hwnd, p);
    memcpy(nm + p, &id, p);
    memcpy(nm + 2 * p, &code, 4);
    memcpy(nm + hs, box, 16);
    w32_send(w->parent, WM_NOTIFY, (WPARAM)w->id, (LPARAM)(uintptr_t)nm);
    w32_free(nm);
}

LRESULT w32_richedit_msg(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp, bool *handled)
{
    *handled = true;
    switch (msg) {
    case EM_STREAMIN: return stream_in(w, wp, (EDITSTREAM *)lp);
    case EM_STREAMOUT: return stream_out(w, wp, (EDITSTREAM *)lp);
    case EM_FINDTEXT:
    case EM_FINDTEXTW:
    case EM_FINDTEXTEX:
    case EM_FINDTEXTEXW:
        return find_text(w, wp, (uint8_t *)lp, msg == EM_FINDTEXTW || msg == EM_FINDTEXTEXW,
                         msg == EM_FINDTEXTEX || msg == EM_FINDTEXTEXW);
    case EM_GETTEXTRANGE: return text_range(w, (uint8_t *)lp);
    case EM_FORMATRANGE: return format_range(w, wp, (uint8_t *)lp);
    case EM_REQUESTRESIZE: request_resize(w); return 0;
    case EM_SETTEXTEX: {
        const DWORD *st = (const DWORD *)wp;
        UINT cp = st ? st[1] : 0;
        bool unicode = cp == 1200;
        if (!lp) {
            set_text(w, w32_zalloc(1), st && (st[0] & 2));
            return 1;
        }
        size_t n = unicode ? w32_wcslen((const WCHAR *)lp) * 2 : strlen((const char *)lp);
        set_text(w, rich_text((const char *)lp, n, false, unicode, cp && cp != 1200 ? (int)cp : w32_acp(), NULL),
                 st && (st[0] & 2));
        return 1;
    }
    case EM_GETTEXTLENGTHEX: return w->text ? (LRESULT)utf8_chars(w->text) : 0;
    case EM_EXLIMITTEXT: w->edit_cap = lp ? (size_t)lp : 65536; return 0;
    case EM_EXGETSEL: {
        size_t from, to;
        w32_edit_selection(w, &from, &to);
        if (lp && w->text) {
            ((LONG *)lp)[0] = (LONG)chars_in(w->text, from);
            ((LONG *)lp)[1] = (LONG)chars_in(w->text, to);
        }
        return 0;
    }
    case EM_EXSETSEL:
        if (lp && w->text) {
            LONG lo = ((const LONG *)lp)[0], hi = ((const LONG *)lp)[1];
            if (lo == 0 && hi == -1)
                w32_edit_select(w, 0, strlen(w->text));
            else if (lo < 0)
                w->has_sel = false;
            else
                w32_edit_select(w, byte_at(w->text, lo), byte_at(w->text, hi));
        }
        return w->text ? (LRESULT)chars_in(w->text, w->caret) : 0;
    case EM_SETBKGNDCOLOR: return 0xffffff;
    case EM_SETEVENTMASK:
    case EM_GETEVENTMASK:
    case EM_GETOLEINTERFACE:
    case EM_HIDESELECTION:
    case EM_SETOPTIONS:
    case EM_GETOPTIONS:
    case EM_AUTOURLDETECT:
    case EM_SETLANGOPTIONS:
    case EM_GETLANGOPTIONS:
    case EM_SETEDITSTYLE:
    case EM_GETEDITSTYLE: return 0;
    case EM_SETCHARFORMAT:
        if (lp)
            set_char_format(w, (const uint8_t *)lp);
        return 1;
    case EM_SETPARAFORMAT: {
        const uint8_t *pf = (const uint8_t *)lp;
        uint32_t mask = 0;
        int16_t count = 0;
        if (pf)
            memcpy(&mask, pf + 4, 4), memcpy(&count, pf + 26, 2);
        if ((mask & 0x10) && count > 0)
            memcpy(&w->rich_style.tab, pf + 28, 4);
        return 1;
    }
    case EM_SETTARGETDEVICE:
    case EM_SETTYPOGRAPHYOPTIONS:
    case EM_SETZOOM: return 1;
    default: *handled = false; return 0;
    }
}

LRESULT WINAPI w32_richedit_proc(HANDLE h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct w32_window *w = w32_win(h);
    if (w && w->control == CK_NONE) {
        w->control = CK_EDIT;
        w->rich = true;
    }
    if (w && w->rich) {
        bool handled;
        LRESULT r = w32_richedit_msg(w, msg, wp, lp, &handled);
        if (handled)
            return r;
    }
    return w32_control_proc(h, msg, wp, lp);
}

static uint32_t WINAPI CreateTextServices(void *outer, void *host, void **out)
{
    UNUSED(outer);
    UNUSED(host);
    if (out)
        *out = NULL;
    return 0x80004001u;
}

static const struct w32_export riched20_exports[] = {
    W32_FN(CreateTextServices),
    W32_AS("REExtendedRegisterClass", w32_zero0),
};
W32_EXPORTS(riched20_module, "riched20.dll", riched20_exports);
#endif
