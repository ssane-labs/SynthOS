#ifndef SYNTH_WIN32_MSIAPI_H
#define SYNTH_WIN32_MSIAPI_H

/* msiexec (kernel/win32/msi.c, x32 code on the 64-bit kernel) and the custom-action module (msica.sxm, ordinary
   64-bit code) share only these calls: numbers and strings, never structures. */

#include <synth/types.h>

struct msi_run;

int msi_api_rows(struct msi_run *r, const char *table); /* -1: no such table */
int msi_api_str(struct msi_run *r, const char *table, int row, int col, char *out, int cap);
int msi_api_int(struct msi_run *r, const char *table, int row, int col);
int msi_api_get(struct msi_run *r, int kind, const char *name, char *out, int cap); /* 0 property, 1 folder, 2 file */
void msi_api_set(struct msi_run *r, const char *name, const char *value, int folder);
void msi_api_format(struct msi_run *r, const char *in, char *out, int cap);
int msi_api_component(struct msi_run *r, const char *comp);
void msi_api_say(struct msi_run *r, const char *text);
int msi_api_save(struct msi_run *r, const char *stream, const char *win);
int msi_api_run(struct msi_run *r, const char *exe, const char *args, const char *workdir);

/* what the module offers as the "msi-actions" service: launch conditions and the actions before the files (an
   msiexec exit code, 0 to go on), then the actions after them (0, or an exit code once its rollback actions ran) */
struct msi_actions {
    K64(int (*before)(struct msi_run *r));
    K64(int (*after)(struct msi_run *r));
};

#endif
