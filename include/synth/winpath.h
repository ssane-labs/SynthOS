#ifndef SYNTH_WINPATH_H
#define SYNTH_WINPATH_H

#include <synth/types.h>

struct winpath_rule {
    const char *windows;
    const char *synth;
};

int winpath_to_vfs(const char *cwd, const char *path, char *out, size_t cap);
void winpath_from_vfs(const char *path, char *out, size_t cap);
void winpath_rules(void (*fn)(const struct winpath_rule *rule, void *ctx), void *ctx);
bool vfs_find_nocase(const char *dir, const char *name, char *out, size_t cap);
int vfs_resolve_nocase(const char *path, char *out, size_t cap);

#endif
