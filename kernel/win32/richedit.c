#include <synth/config.h>
#if CONFIG_GUI
#include <synth/string.h>

#include "w32gui.h"

/* RichEdit: a multi-line edit control that takes RTF; formatting is dropped, the text is kept */
enum {
    EM_EXGETSEL = 0x434,
    EM_EXLIMITTEXT = 0x435,
    EM_EXSETSEL = 0x437,
    EM_GETEVENTMASK = 0x43b,
    EM_GETOLEINTERFACE = 0x43c,
    EM_HIDESELECTION = 0x43f,
    EM_SETBKGNDCOLOR = 0x443,
    EM_SETCHARFORMAT = 0x444,
    EM_SETEVENTMASK = 0x445,
    EM_SETPARAFORMAT = 0x447,
    EM_SETTARGETDEVICE = 0x448,
    EM_STREAMIN = 0x449,
    EM_STREAMOUT = 0x44a,
    EM_SETOPTIONS = 0x44d,
    EM_GETOPTIONS = 0x44e,
    EM_AUTOURLDETECT = 0x45b,
    EM_GETTEXTEX = 0x45e,
    EM_GETTEXTLENGTHEX = 0x45f,
    EM_SETTEXTEX = 0x461,
    EM_SETLANGOPTIONS = 0x478,
    EM_GETLANGOPTIONS = 0x479,
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

static void rtf_to_text(const char *s, size_t n, struct sbuf *out)
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
        if (skipped_destination(word)) {
            skip = depth;
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
static char *rich_text(const char *data, size_t n, bool rtf, bool unicode, int codepage)
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
        rtf_to_text(data, n, &out);
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

static LRESULT stream_in(struct w32_window *w, WPARAM flags, EDITSTREAM *es)
{
    if (!es || !es->callback)
        return 0;
    struct sbuf raw = {0};
    char *buf = w32_alloc(4096);
    LONG *got = w32_alloc(sizeof(LONG));
    if (!buf || !got) {
        w32_free(buf);
        w32_free(got);
        return 0;
    }
    es->error = 0;
    while (raw.n < RTF_MAX && !raw.failed) {
        *got = 0;
        DWORD r = W32_CALL(DWORD(WINAPI *)(uintptr_t, void *, LONG, LONG *), es->callback, es->cookie, buf, 4096,
                           got);
        if (r) {
            es->error = r;
            break;
        }
        if (*got <= 0)
            break;
        sb_put(&raw, buf, (size_t)MIN(*got, 4096));
    }
    w32_free(buf);
    w32_free(got);
    int codepage = flags & SF_USECODEPAGE ? (int)(flags >> 16) : w32_acp();
    char *text = raw.p ? rich_text(raw.p, raw.n, flags & SF_RTF, (flags & SF_UNICODE) || codepage == 1200, codepage)
                       : NULL;
    w32_free(raw.p);
    if (!text)
        text = w32_zalloc(1);
    LRESULT count = text ? (LRESULT)strlen(text) : 0;
    set_text(w, text, flags & SFF_SELECTION);
    return count;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++)
        n += ((uint8_t)*s & 0xc0) != 0x80;
    return n;
}

LRESULT w32_richedit_msg(struct w32_window *w, UINT msg, WPARAM wp, LPARAM lp, bool *handled)
{
    *handled = true;
    switch (msg) {
    case EM_STREAMIN: return stream_in(w, wp, (EDITSTREAM *)lp);
    case EM_STREAMOUT: return 0;
    case EM_SETTEXTEX: {
        const DWORD *st = (const DWORD *)wp;
        UINT cp = st ? st[1] : 0;
        bool unicode = cp == 1200;
        if (!lp) {
            set_text(w, w32_zalloc(1), st && (st[0] & 2));
            return 1;
        }
        size_t n = unicode ? w32_wcslen((const WCHAR *)lp) * 2 : strlen((const char *)lp);
        set_text(w, rich_text((const char *)lp, n, false, unicode, cp && cp != 1200 ? (int)cp : w32_acp()),
                 st && (st[0] & 2));
        return 1;
    }
    case EM_GETTEXTLENGTHEX: return w->text ? (LRESULT)utf8_chars(w->text) : 0;
    case EM_EXLIMITTEXT: w->edit_cap = lp ? (size_t)lp : 65536; return 0;
    case EM_EXGETSEL:
        if (lp)
            ((LONG *)lp)[0] = ((LONG *)lp)[1] = (LONG)w->caret;
        return 0;
    case EM_EXSETSEL:
        if (lp && w->text) {
            LONG at = ((const LONG *)lp)[1];
            size_t len = strlen(w->text);
            w->caret = at < 0 ? len : MIN((size_t)at, len);
        }
        return (LRESULT)w->caret;
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
    case EM_SETPARAFORMAT:
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
