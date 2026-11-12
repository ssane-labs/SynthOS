/* msvcp60.dll: the std::string (basic_string<char>) members old Visual C++ 6 programs import (Notepad2).
   The VC6 layout: an allocator byte, the text, its length and capacity; the byte before the text is a share
   count, kept at 0 (every string owns its buffer). Buffers come from msvcrt, so the program's inline code can
   free them with operator delete. */
#include <stdlib.h>
#include <string.h>

#define THIS __attribute__((thiscall))
#define NPOS 0xffffffffu

struct str {
    char alloc;
    char *ptr;
    unsigned len, res;
};

const char nullstr_char = 0;
const unsigned npos_value = NPOS;

static int reserve(struct str *s, unsigned n)
{
    if (s->ptr && n <= s->res)
        return 1;
    unsigned cap = n | 31;
    char *b = malloc(cap + 2);
    if (!b)
        return 0;
    b[0] = 0;
    if (s->ptr) {
        memcpy(b + 1, s->ptr, s->len + 1);
        free(s->ptr - 1);
    } else {
        b[1] = 0;
    }
    s->ptr = b + 1;
    s->res = cap;
    return 1;
}

static void tidy(struct str *s)
{
    if (s->ptr)
        free(s->ptr - 1);
    s->ptr = NULL;
    s->len = s->res = 0;
}

static const char *text(const struct str *s)
{
    return s->ptr ? s->ptr : "";
}

/* replaces count bytes at pos with n bytes from p (p may point into the string itself) */
static struct str *splice(struct str *s, unsigned pos, unsigned count, const char *p, unsigned n)
{
    if (pos > s->len)
        pos = s->len;
    if (count > s->len - pos)
        count = s->len - pos;
    char *copy = NULL;
    if (n && s->ptr && p >= s->ptr && p < s->ptr + s->res + 1) {
        copy = malloc(n);
        if (!copy)
            return s;
        memcpy(copy, p, n);
        p = copy;
    }
    unsigned len = s->len - count + n;
    if (reserve(s, len)) {
        memmove(s->ptr + pos + n, s->ptr + pos + count, s->len - pos - count + 1);
        if (n)
            memcpy(s->ptr + pos, p, n);
        s->len = len;
    }
    free(copy);
    return s;
}

static struct str *assign(struct str *s, const char *p, unsigned n)
{
    return splice(s, 0, s->len, p, n);
}

static int compare(const struct str *a, const char *b, unsigned n)
{
    unsigned m = a->len < n ? a->len : n;
    int c = m ? memcmp(text(a), b, m) : 0;
    return c ? c : a->len < n ? -1 : a->len > n;
}

THIS struct str *str_new(struct str *s, const void *alloc)
{
    (void)alloc;
    s->ptr = NULL;
    s->len = s->res = 0;
    return s;
}

THIS struct str *str_copy(struct str *s, const struct str *from)
{
    str_new(s, NULL);
    return assign(s, text(from), from->len);
}

THIS struct str *str_sub(struct str *s, const struct str *from, unsigned pos, unsigned n, const void *alloc)
{
    str_new(s, alloc);
    if (pos > from->len)
        pos = from->len;
    if (n > from->len - pos)
        n = from->len - pos;
    return assign(s, text(from) + pos, n);
}

THIS struct str *str_fill(struct str *s, unsigned n, char c, const void *alloc)
{
    str_new(s, alloc);
    if (reserve(s, n)) {
        memset(s->ptr, c, n);
        s->ptr[n] = 0;
        s->len = n;
    }
    return s;
}

THIS struct str *str_from(struct str *s, const char *p, const void *alloc)
{
    str_new(s, alloc);
    return assign(s, p, p ? strlen(p) : 0);
}

THIS struct str *str_from_n(struct str *s, const char *p, unsigned n, const void *alloc)
{
    str_new(s, alloc);
    return assign(s, p, n);
}

THIS void str_free(struct str *s)
{
    tidy(s);
}

THIS struct str *str_set(struct str *s, const struct str *from)
{
    return s == from ? s : assign(s, text(from), from->len);
}

THIS struct str *str_set_text(struct str *s, const char *p)
{
    return assign(s, p, p ? strlen(p) : 0);
}

THIS char *str_at(struct str *s, unsigned pos)
{
    if (!reserve(s, s->len))
        return (char *)&nullstr_char;
    return s->ptr + (pos <= s->len ? pos : s->len);
}

THIS struct str *str_add_char(struct str *s, char c)
{
    return splice(s, s->len, 0, &c, 1);
}

THIS struct str *str_add(struct str *s, const char *p)
{
    return splice(s, s->len, 0, p, p ? strlen(p) : 0);
}

THIS struct str *str_erase(struct str *s, unsigned pos, unsigned n)
{
    return splice(s, pos, n, "", 0);
}

THIS struct str *str_insert(struct str *s, unsigned pos, const char *p, unsigned n)
{
    return splice(s, pos, 0, p, n);
}

THIS unsigned str_find(const struct str *s, const char *p, unsigned pos, unsigned n)
{
    if (pos > s->len)
        return NPOS;
    if (!n)
        return pos;
    for (unsigned i = pos; i + n <= s->len; i++)
        if (!memcmp(s->ptr + i, p, n))
            return i;
    return NPOS;
}

THIS unsigned str_find_text(const struct str *s, const char *p, unsigned pos)
{
    return str_find(s, p, pos, (unsigned)strlen(p));
}

THIS void *lockit_new(void *self)
{
    return self;
}

THIS void lockit_free(void *self)
{
    (void)self;
}

int str_eq_text(const struct str *a, const char *b)
{
    return !compare(a, b, strlen(b));
}

int str_ne(const struct str *a, const struct str *b)
{
    return compare(a, text(b), b->len) != 0;
}

int str_ne_text(const struct str *a, const char *b)
{
    return !str_eq_text(a, b);
}

int str_lt(const struct str *a, const struct str *b)
{
    return compare(a, text(b), b->len) < 0;
}
