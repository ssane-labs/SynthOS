#include <synth/error.h>
#include <synth/heap.h>
#include <synth/lnk.h>
#include <synth/string.h>
#include <synth/vfs.h>

enum {
    HAS_ID_LIST = 0x1,
    HAS_LINK_INFO = 0x2,
    HAS_NAME = 0x4,
    HAS_RELATIVE_PATH = 0x8,
    HAS_WORKING_DIR = 0x10,
    HAS_ARGUMENTS = 0x20,
    HAS_ICON_LOCATION = 0x40,
    IS_UNICODE = 0x80,
};

static const uint8_t shell_link_clsid[16] = {0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};

struct writer {
    uint8_t *p;
    size_t n, cap;
};

static void put(struct writer *w, const void *data, size_t n)
{
    if (w->n + n <= w->cap)
        memcpy(w->p + w->n, data, n);
    w->n += n;
}

static void put32(struct writer *w, uint32_t v)
{
    put(w, &v, 4);
}

static void put16(struct writer *w, uint16_t v)
{
    put(w, &v, 2);
}

static void put_at32(struct writer *w, size_t at, uint32_t v)
{
    if (at + 4 <= w->cap)
        memcpy(w->p + at, &v, 4);
}

static uint32_t next_utf8(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : extra == 1 ? 0x1f : 0x7f;
    while (extra-- > 0 && (*p & 0xc0) == 0x80)
        c = c << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return c;
}

static size_t wide_len(const char *s)
{
    size_t n = 0;
    while (*s) {
        next_utf8(&s);
        n++;
    }
    return n;
}

static void put_wide(struct writer *w, const char *s, bool terminate)
{
    while (*s) {
        uint32_t c = next_utf8(&s);
        put16(w, c > 0xffff ? '?' : (uint16_t)c);
    }
    if (terminate)
        put16(w, 0);
}

static void put_ansi(struct writer *w, const char *s)
{
    while (*s) {
        uint32_t c = next_utf8(&s);
        uint8_t b = c < 0x80 ? (uint8_t)c : '?';
        put(w, &b, 1);
    }
    put(w, "", 1);
}

static void put_string(struct writer *w, const char *s)
{
    put16(w, (uint16_t)wide_len(s));
    put_wide(w, s, false);
}

size_t lnk_encode(const struct lnk *l, uint8_t *out, size_t cap)
{
    struct writer w = {out, 0, cap};
    uint32_t flags = HAS_LINK_INFO | IS_UNICODE | (l->desc[0] ? HAS_NAME : 0) | (l->dir[0] ? HAS_WORKING_DIR : 0) |
                     (l->args[0] ? HAS_ARGUMENTS : 0) | (l->icon[0] ? HAS_ICON_LOCATION : 0);
    put32(&w, 0x4c);
    put(&w, shell_link_clsid, 16);
    put32(&w, flags);
    put32(&w, 0x20);
    uint8_t zero[24] = {0};
    put(&w, zero, 24);
    put32(&w, 0);
    put32(&w, (uint32_t)l->icon_index);
    put32(&w, l->show ? (uint32_t)l->show : 1);
    put(&w, zero, 12);

    size_t info = w.n;
    put32(&w, 0);
    put32(&w, 0x24);
    put32(&w, 1);
    put32(&w, 0x24);
    put32(&w, 0);
    put32(&w, 0);
    put32(&w, 0);
    put32(&w, 0);
    put32(&w, 0);
    put32(&w, 0x11);
    put32(&w, 3);
    put32(&w, 0x5354594e);
    put32(&w, 0x10);
    put(&w, "", 1);
    put_at32(&w, info + 16, (uint32_t)(w.n - info));
    put_ansi(&w, l->target);
    put_at32(&w, info + 24, (uint32_t)(w.n - info));
    put(&w, "", 1);
    put_at32(&w, info + 28, (uint32_t)(w.n - info));
    put_wide(&w, l->target, true);
    put_at32(&w, info + 32, (uint32_t)(w.n - info));
    put16(&w, 0);
    put_at32(&w, info, (uint32_t)(w.n - info));

    if (flags & HAS_NAME)
        put_string(&w, l->desc);
    if (flags & HAS_WORKING_DIR)
        put_string(&w, l->dir);
    if (flags & HAS_ARGUMENTS)
        put_string(&w, l->args);
    if (flags & HAS_ICON_LOCATION)
        put_string(&w, l->icon);
    put32(&w, 0);
    return w.n <= cap ? w.n : 0;
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void append_utf8(char *out, size_t cap, size_t *len, uint32_t c)
{
    char buf[4];
    size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (n == 1) {
        buf[0] = (char)c;
    } else if (n == 2) {
        buf[0] = (char)(0xc0 | c >> 6);
        buf[1] = (char)(0x80 | (c & 0x3f));
    } else {
        buf[0] = (char)(0xe0 | c >> 12);
        buf[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (c & 0x3f));
    }
    if (*len + n < cap) {
        memcpy(out + *len, buf, n);
        *len += n;
    }
    out[*len] = 0;
}

static void copy_ansi(const uint8_t *p, const uint8_t *end, char *out, size_t cap, size_t *len)
{
    for (; p < end && *p; p++)
        append_utf8(out, cap, len, *p);
}

static void copy_wide(const uint8_t *p, const uint8_t *end, char *out, size_t cap, size_t *len)
{
    for (; p + 1 < end && get16(p); p += 2)
        append_utf8(out, cap, len, get16(p));
}

static bool read_string(const uint8_t **p, const uint8_t *end, bool unicode, char *out, size_t cap)
{
    if (*p + 2 > end)
        return false;
    size_t count = get16(*p), size = count * (unicode ? 2 : 1), len = 0;
    *p += 2;
    if (*p + size > end)
        return false;
    out[0] = 0;
    for (size_t i = 0; i < count; i++)
        append_utf8(out, cap, &len, unicode ? get16(*p + i * 2) : (*p)[i]);
    *p += size;
    return true;
}

int lnk_decode(const uint8_t *data, size_t size, struct lnk *l)
{
    memset(l, 0, sizeof(*l));
    if (size < 0x4c || get32(data) != 0x4c || memcmp(data + 4, shell_link_clsid, 16))
        return E_INVAL;
    uint32_t flags = get32(data + 20);
    l->icon_index = (int)get32(data + 56);
    l->show = (int)get32(data + 60);
    const uint8_t *p = data + 0x4c, *end = data + size;
    if (flags & HAS_ID_LIST) {
        if (p + 2 > end)
            return E_INVAL;
        p += 2 + get16(p);
    }
    if (flags & HAS_LINK_INFO) {
        if (p + 28 > end || p + get32(p) > end)
            return E_INVAL;
        const uint8_t *info = p, *info_end = p + get32(p);
        uint32_t header = get32(info + 4), info_flags = get32(info + 8);
        size_t len = 0;
        if (info_flags & 1) {
            if (header >= 0x24 && get32(info + 28)) {
                copy_wide(info + get32(info + 28), info_end, l->target, sizeof(l->target), &len);
                if (get32(info + 32))
                    copy_wide(info + get32(info + 32), info_end, l->target, sizeof(l->target), &len);
            } else {
                copy_ansi(info + get32(info + 16), info_end, l->target, sizeof(l->target), &len);
                copy_ansi(info + get32(info + 24), info_end, l->target, sizeof(l->target), &len);
            }
        }
        p = info_end;
    }
    bool unicode = flags & IS_UNICODE;
    char relative[260] = "";
    if ((flags & HAS_NAME) && !read_string(&p, end, unicode, l->desc, sizeof(l->desc)))
        return E_INVAL;
    if ((flags & HAS_RELATIVE_PATH) && !read_string(&p, end, unicode, relative, sizeof(relative)))
        return E_INVAL;
    if ((flags & HAS_WORKING_DIR) && !read_string(&p, end, unicode, l->dir, sizeof(l->dir)))
        return E_INVAL;
    if ((flags & HAS_ARGUMENTS) && !read_string(&p, end, unicode, l->args, sizeof(l->args)))
        return E_INVAL;
    if ((flags & HAS_ICON_LOCATION) && !read_string(&p, end, unicode, l->icon, sizeof(l->icon)))
        return E_INVAL;
    if (!l->target[0])
        str_copy(l->target, relative, sizeof(l->target));
    return l->target[0] ? 0 : E_INVAL;
}

int lnk_read(const char *path, struct lnk *l)
{
    char *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size);
    if (err)
        return err;
    err = size > 1u << 20 ? E_INVAL : lnk_decode((const uint8_t *)data, size, l);
    kfree(data);
    return err;
}
