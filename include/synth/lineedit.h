#ifndef SYNTH_LINEEDIT_H
#define SYNTH_LINEEDIT_H

#include <synth/shell.h>

enum {
    LE_EOF = -1,
    LE_CANCEL = -2,
};

struct completion {
    size_t start;
    char **items;
    size_t count;
    size_t cap;
};

struct lineedit_hooks {
    void *ctx;
    void (*prompt)(void *ctx);
    void (*highlight)(void *ctx, const char *buf, size_t len, uint8_t *attrs);
    void (*complete)(void *ctx, const char *buf, size_t cursor, struct completion *out);
    struct history *history;
    bool suggest;
};

int lineedit_read(char *buf, size_t cap, const struct lineedit_hooks *hooks);
int lineedit_simple(char *buf, size_t cap);
void completion_add(struct completion *c, const char *item);
void completion_free(struct completion *c);

#endif
