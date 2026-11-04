#ifndef SYNTH_W32_DLGTMPL_H
#define SYNTH_W32_DLGTMPL_H

#include <synth/string.h>

#include "w32gui.h"

/* in-memory DLGTEMPLATE building for the built-in common dialogs */
struct tmpl {
    uint8_t *p;
    size_t n;
};

static inline void put16(struct tmpl *t, uint16_t v)
{
    memcpy(t->p + t->n, &v, 2);
    t->n += 2;
}

static inline void put32(struct tmpl *t, uint32_t v)
{
    memcpy(t->p + t->n, &v, 4);
    t->n += 4;
}

static inline void put_str(struct tmpl *t, const char *s)
{
    while (*s)
        put16(t, (uint8_t)*s++);
    put16(t, 0);
}

static inline void item(struct tmpl *t, uint32_t style, int x, int y, int w, int h, int id, uint16_t cls, const char *text)
{
    t->n = (t->n + 3) & ~(size_t)3;
    put32(t, style | WS_CHILD | WS_VISIBLE);
    put32(t, 0);
    put16(t, (uint16_t)x);
    put16(t, (uint16_t)y);
    put16(t, (uint16_t)w);
    put16(t, (uint16_t)h);
    put16(t, (uint16_t)id);
    put16(t, 0xffff);
    put16(t, cls);
    put_str(t, text);
    put16(t, 0);
}

static inline void item_class(struct tmpl *t, uint32_t style, int x, int y, int w, int h, int id, const char *cls,
                              const char *text)
{
    t->n = (t->n + 3) & ~(size_t)3;
    put32(t, style | WS_CHILD | WS_VISIBLE);
    put32(t, 0);
    put16(t, (uint16_t)x);
    put16(t, (uint16_t)y);
    put16(t, (uint16_t)w);
    put16(t, (uint16_t)h);
    put16(t, (uint16_t)id);
    put_str(t, cls);
    put_str(t, text);
    put16(t, 0);
}

static inline void dialog_header(struct tmpl *t, uint32_t style, int count, int w, int h, const char *title)
{
    put32(t, style);
    put32(t, 0);
    put16(t, (uint16_t)count);
    put16(t, 30);
    put16(t, 30);
    put16(t, (uint16_t)w);
    put16(t, (uint16_t)h);
    put16(t, 0);
    put16(t, 0);
    put_str(t, title);
    put16(t, 8);
    put_str(t, "MS Shell Dlg");
}

#define DLG_CALL(dll, name, type) ((type)w32_builtin_lookup(dll, name))

#endif
