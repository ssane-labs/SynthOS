#ifndef SYNTH_PATH_H
#define SYNTH_PATH_H

#include <synth/types.h>

#define PATH_MAX_LEN 256
#define NAME_MAX_LEN 64

int path_resolve(const char *cwd, const char *path, char *out, size_t cap);
int path_join(const char *dir, const char *name, char *out, size_t cap);
const char *path_basename(const char *path);
void path_dirname(const char *path, char *out, size_t cap);
const char *path_ext(const char *path);
bool wildcard_match(const char *pattern, const char *s, bool icase);
bool has_wildcard(const char *s);

#endif
