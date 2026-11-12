#ifndef SYNTH_VERSIONS_H
#define SYNTH_VERSIONS_H

#include <synth/types.h>

struct vnode;

extern bool versions_ready;
bool versions_tracked(const char *path);
void versions_saved(const char *path);
void versions_keep(const char *path, struct vnode *vn);
int versions_list(const char *path, uint32_t *times, uint32_t *sizes, int max);
int versions_restore(const char *path, uint32_t time);

#endif
